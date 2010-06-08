/*
    Copyright 2008 Brain Research Institute, Melbourne, Australia

    Written by J-Donald Tournier, 27/06/08.

    This file is part of MRtrix.

    MRtrix is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    MRtrix is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with MRtrix.  If not, see <http://www.gnu.org/licenses/>.

*/

#include "app.h"
#include "progressbar.h"
#include "image/voxel.h"
#include "math/matrix.h"
#include "math/eigen.h"
#include "dwi/tensor.h"

using namespace std; 
using namespace MR; 

SET_VERSION_DEFAULT;

DESCRIPTION = {
  "generate maps of tensor-derived parameters.",
  NULL
};

ARGUMENTS = {
  Argument ("tensor", "input tensor image", "the input diffusion tensor image.").type_image_in (),
  Argument::End
};

const char* modulate_choices[] = { "none", "fa", "eval", NULL };

OPTIONS = { 
  Option ("adc", "mean ADC", "compute the mean apparent diffusion coefficient (ADC) of the diffusion tensor.")
    .append (Argument ("image", "image", "the output mean ADC image.").type_image_out ()),

  Option ("fa", "fractional anisotropy", "compute the fractional anisotropy of the diffusion tensor.")
    .append (Argument ("image", "image", "the output fractional anisotropy image.").type_image_out ()),

  Option ("num", "numbers", "specify the desired eigenvalue/eigenvector(s). Note that several eigenvalues can be specified as a number sequence. For example, '1,3' specifies the major (1) and minor (3) eigenvalues/eigenvectors (default = 1).")
    .append (Argument ("image", "image", "the mask image to use.").type_string ()),

  Option ("vector", "eigenvector", "compute the selected eigenvector(s) of the diffusion tensor.")
    .append (Argument ("image", "image", "the output eigenvector image.").type_image_out ()),

  Option ("value", "eigenvalue", "compute the selected eigenvalue(s) of the diffusion tensor.")
    .append (Argument ("image", "image", "the output eigenvalue image.").type_image_out ()),

  Option ("mask", "brain mask", "only perform computation within the specified binary brain mask image.")
    .append (Argument ("image", "image", "the mask image to use.").type_image_in ()),

  Option ("modulate", "modulate vector", "specify how to modulate the magnitude of the eigenvectors. Valid choices are: none, FA, eval (default = FA).")
    .append (Argument ("spec", "specifier", "the modulation specifier.").type_choice (modulate_choices)),

  Option::End 
};


class ImagePair
{
  public:
    ImagePair (const Image::Header& header) : H (header), vox (H) { }
    Image::Header H;
    Image::Voxel<float> vox;
};

inline void set_zero (size_t axis, Ptr<ImagePair>& i0, Ptr<ImagePair>& i1, Ptr<ImagePair>& i2, Ptr<ImagePair>& i3, Ptr<ImagePair>& i4)
{
  if (i0) i0->vox[axis] = 0; 
  if (i1) i1->vox[axis] = 0; 
  if (i2) i2->vox[axis] = 0; 
  if (i3) i3->vox[axis] = 0; 
  if (i4) i4->vox[axis] = 0; 
}

inline void increment (size_t axis, Ptr<ImagePair>& i0, Ptr<ImagePair>& i1, Ptr<ImagePair>& i2, Ptr<ImagePair>& i3, Ptr<ImagePair>& i4)
{
  if (i0) ++i0->vox[axis]; 
  if (i1) ++i1->vox[axis]; 
  if (i2) ++i2->vox[axis]; 
  if (i3) ++i3->vox[axis]; 
  if (i4) ++i4->vox[axis]; 
}

EXECUTE {
  Image::Header header (argument[0].get_image());
  Image::Voxel<float> dt (header);

  if (header.ndim() != 4) 
    throw Exception ("base image should contain 4 dimensions");

  if (header.dim(3) != 6) 
    throw Exception ("expecting dimension 3 of image \"" + header.name() + "\" to be 6");

  header.datatype() = DataType::Float32;
  std::vector<OptBase> opt;

  std::vector<int> vals(1);
  vals[0] = 1;
  opt = get_options ("num");
  if (opt.size()) {
    vals = parse_ints (opt[0][0].get_string());
    if (vals.empty()) throw Exception ("invalid eigenvalue/eigenvector number specifier");
    for (size_t i = 0; i < vals.size(); i++)
      if (vals[i] < 1 || vals[i] > 3) throw Exception ("eigenvalue/eigenvector number is out of bounds");
  }

  Ptr<ImagePair> adc, fa, eval, evec, mask;

  opt = get_options ("vector");
  if (opt.size()) {
    header.axes.dim(3) = 3*vals.size();
    evec = new ImagePair (opt[0][0].get_image (header));
  }

  opt = get_options ("value");
  if (opt.size()) {
    header.axes.dim(3) = vals.size();
    eval = new ImagePair (opt[0][0].get_image (header));
  }

  header.axes.ndim() = 3;
  opt = get_options ("adc");
  if (opt.size()) adc = new ImagePair (opt[0][0].get_image (header));

  opt = get_options ("fa");
  if (opt.size()) fa = new ImagePair (opt[0][0].get_image (header));

  opt = get_options ("mask");
  if (opt.size()) {
    mask = new ImagePair (opt[0][0].get_image ());
    if (mask->H.dim(0) != dt.dim(0) || mask->H.dim(1) != dt.dim(1) || mask->H.dim(2) != dt.dim(2)) 
      throw Exception ("dimensions of mask image do not match that of tensor image - aborting");
  }

  int modulate = 1;
  opt = get_options ("modulate");
  if (opt.size()) modulate = opt[0][0].get_int();

  if ( ! (adc || fa || eval || evec))
    throw Exception ("no output metric specified - aborting");


  for (size_t i = 0; i < vals.size(); i++)
    vals[i] = 3-vals[i];
 

  Math::Matrix<double> V(3,3), M(3,3);
  Math::Vector<double> ev(3);
  float el[6], faval = NAN;

  Ptr<Math::Eigen::Symm<double> > eig;
  Ptr<Math::Eigen::SymmV<double> > eigv;
  if (evec) eigv = new Math::Eigen::SymmV<double> (3);
  else eig = new Math::Eigen::Symm<double> (3);

  ProgressBar progress ("computing tensor metrics...", DataSet::voxel_count (dt, 0, 3));

  for (dt[2] = 0; dt[2] < dt.dim(2); dt[2]++) {
    set_zero (1, mask, fa, adc, eval, evec);

    for (dt[1] = 0; dt[1] < dt.dim(1); dt[1]++) {
      set_zero (0, mask, fa, adc, eval, evec);

      for (dt[0] = 0; dt[0] < dt.dim(0); dt[0]++) {

        bool skip = false;
        if (mask) if (mask->vox.value() < 0.5) skip = true;

        if (!skip) {

          for (dt[3] = 0; dt[3] < dt.dim(3); dt[3]++) 
            el[dt[3]] = dt.value();

          if (adc) adc->vox.value() = DWI::tensor2ADC (el);
          if (fa || modulate == 1) faval = DWI::tensor2FA (el);
          if (fa) fa->vox.value() = faval;

          if (eval || evec) {
            M(0,0) = el[0];
            M(1,1) = el[1];
            M(2,2) = el[2];
            M(0,1) = M(1,0) = el[3];
            M(0,2) = M(2,0) = el[4];
            M(1,2) = M(2,1) = el[5];

            if (evec) {
              (*eigv) (ev, M, V);
              Math::Eigen::sort (ev, V);
              if (modulate == 0) faval = 1.0;
              evec->vox[3] = 0;
              for (size_t i = 0; i < vals.size(); i++) {
                if (modulate == 2) faval = ev[vals[i]];
                evec->vox.value() = faval*V(0,vals[i]); ++evec->vox[3];
                evec->vox.value() = faval*V(1,vals[i]); ++evec->vox[3];
                evec->vox.value() = faval*V(2,vals[i]); ++evec->vox[3];
              }
            }
            else {
              (*eig) (ev, M);
              Math::Eigen::sort (ev);
            }

            if (eval) {
              for (eval->vox[3] = 0; eval->vox[3] < (int) vals.size(); ++eval->vox[3])
                eval->vox.value() = ev[vals[eval->vox[3]]]; 
            }
          }
        }

        ++progress;
        increment (0, mask, fa, adc, eval, evec);
      }
      increment (1, mask, fa, adc, eval, evec);
    }
    increment (2, mask, fa, adc, eval, evec);
  }
}
