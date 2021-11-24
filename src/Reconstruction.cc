/*
 * SVRTK : SVR reconstruction based on MIRTK
 *
 * Copyright 2008-2017 Imperial College London
 * Copyright 2018-2021 King's College London
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// SVRTK
#include "svrtk/Reconstruction.h"
#include "svrtk/Profiling.h"
#include "svrtk/Parallel.h"
#include "svrtk/Utility.h"

using namespace std;
using namespace mirtk;
using namespace svrtk::Utility;

namespace svrtk {

    // Extract specific image ROI
    static void bbox(RealImage& stack, RigidTransformation& transformation, double& min_x, double& min_y, double& min_z, double& max_x, double& max_y, double& max_z) {
        min_x = DBL_MAX;
        min_y = DBL_MAX;
        min_z = DBL_MAX;
        max_x = -DBL_MAX;
        max_y = -DBL_MAX;
        max_z = -DBL_MAX;
        // WARNING: do not search to increment by stack.GetZ()-1,
        // otherwise you would end up with a 0 increment for slices...
        for (int i = 0; i <= stack.GetX(); i += stack.GetX())
            for (int j = 0; j <= stack.GetY(); j += stack.GetY())
                for (int k = 0; k <= stack.GetZ(); k += stack.GetZ()) {
                    double x = i;
                    double y = j;
                    double z = k;

                    stack.ImageToWorld(x, y, z);
                    transformation.Transform(x, y, z);

                    if (x < min_x)
                        min_x = x;
                    if (y < min_y)
                        min_y = y;
                    if (z < min_z)
                        min_z = z;
                    if (x > max_x)
                        max_x = x;
                    if (y > max_y)
                        max_y = y;
                    if (z > max_z)
                        max_z = z;
                }
    }

    //-------------------------------------------------------------------

    // Crop to non zero ROI
    static void bboxCrop(RealImage& image) {
        int min_x = image.GetX() - 1;
        int min_y = image.GetY() - 1;
        int min_z = image.GetZ() - 1;
        int max_x = 0;
        int max_y = 0;
        int max_z = 0;
        for (int i = 0; i < image.GetX(); i++)
            for (int j = 0; j < image.GetY(); j++)
                for (int k = 0; k < image.GetZ(); k++) {
                    if (image.Get(i, j, k) > 0) {
                        if (i < min_x)
                            min_x = i;
                        if (j < min_y)
                            min_y = j;
                        if (k < min_z)
                            min_z = k;
                        if (i > max_x)
                            max_x = i;
                        if (j > max_y)
                            max_y = j;
                        if (k > max_z)
                            max_z = k;
                    }
                }

        //Cut region of interest
        image = image.GetRegion(min_x, min_y, min_z, max_x, max_y, max_z);
    }

    //-------------------------------------------------------------------

    // Find centroid
    static void centroid(RealImage& image, double& x, double& y, double& z) {
        double sum_x = 0;
        double sum_y = 0;
        double sum_z = 0;
        double norm = 0;
        for (int i = 0; i < image.GetX(); i++)
            for (int j = 0; j < image.GetY(); j++)
                for (int k = 0; k < image.GetZ(); k++) {
                    double v = image.Get(i, j, k);
                    if (v <= 0)
                        continue;
                    sum_x += v * i;
                    sum_y += v * j;
                    sum_z += v * k;
                    norm += v;
                }

        x = sum_x / norm;
        y = sum_y / norm;
        z = sum_z / norm;

        image.ImageToWorld(x, y, z);
    }

    //-------------------------------------------------------------------

    Reconstruction::Reconstruction() {
        _number_of_slices_org = 0;
        _average_thickness_org = 0;
        _cp_spacing = -1;

        _step = 0.0001;
        _debug = false;
        _verbose = false;
        _quality_factor = 2;
        _sigma_bias = 12;
        _sigma_s = 0.025;
        _sigma_s2 = 0.025;
        _mix_s = 0.9;
        _mix = 0.9;
        _delta = 1;
        _lambda = 0.1;
        _alpha = (0.05 / _lambda) * _delta * _delta;
        _low_intensity_cutoff = 0.01;
        _nmi_bins = -1;
        _global_NCC_threshold = 0.65;

        _template_created = false;
        _have_mask = false;
        _global_bias_correction = false;
        _adaptive = false;
        _robust_slices_only = false;
        _recon_type = _3D;
        _ffd = false;
        _blurring = false;
        _structural = false;
        _ncc_reg = false;
        _template_flag = false;
        _no_sr = false;
        _reg_log = false;
        _masked_stacks = false;
        _filtered_cmp_flag = false;
        _bg_flag = false;
    }

    //-------------------------------------------------------------------

    Reconstruction::~Reconstruction() {}

    //-------------------------------------------------------------------

    // Center stacks with respect to the centre of the mask
    void Reconstruction::CenterStacks(const Array<RealImage>& stacks, Array<RigidTransformation>& stack_transformations, int templateNumber) {
        RealImage mask = stacks[templateNumber];
        RealPixel *ptr = mask.Data();
        #pragma omp parallel for
        for (int i = 0; i < mask.NumberOfVoxels(); i++)
            if (ptr[i] < 0)
                ptr[i] = 0;

        double x0, y0, z0;
        centroid(mask, x0, y0, z0);

        #pragma omp parallel for private(mask, ptr)
        for (size_t i = 0; i < stacks.size(); i++) {
            if (i == templateNumber)
                continue;

            mask = stacks[i];
            ptr = mask.Data();
            for (int i = 0; i < mask.NumberOfVoxels(); i++)
                if (ptr[i] < 0)
                    ptr[i] = 0;

            double x, y, z;
            centroid(mask, x, y, z);

            RigidTransformation translation;
            translation.PutTranslationX(x0 - x);
            translation.PutTranslationY(y0 - y);
            translation.PutTranslationZ(z0 - z);

            stack_transformations[i].PutMatrix(translation.GetMatrix() * stack_transformations[i].GetMatrix());
        }
    }

    //-------------------------------------------------------------------

    // Create average of all stacks based on the input transformations
    RealImage Reconstruction::CreateAverage(const Array<RealImage>& stacks, Array<RigidTransformation>& stack_transformations) {
        SVRTK_START_TIMING();

        if (!_template_created) {
            cerr << "Please create the template before calculating the average of the stacks." << endl;
            exit(1);
        }

        InvertStackTransformations(stack_transformations);
        Parallel::Average p_average(this, stacks, stack_transformations, -1, 0, 0, true);
        p_average();
        const RealImage average = p_average.average / p_average.weights;

        InvertStackTransformations(stack_transformations);

        SVRTK_END_TIMING("CreateAverage");
        return average;
    }

    //-------------------------------------------------------------------

    // Set given template with specific resolution
    double Reconstruction::CreateTemplate(const RealImage& stack, double resolution) {
        double dx, dy, dz, d;

        //Get image attributes - image size and voxel size
        ImageAttributes attr = stack.Attributes();

        //enlarge stack in z-direction in case top of the head is cut off
        attr._z += 2;

        //create enlarged image
        RealImage enlarged(attr);

        //determine resolution of volume to reconstruct
        if (resolution <= 0) {
            //resolution was not given by user set it to min of res in x or y direction
            stack.GetPixelSize(&dx, &dy, &dz);
            if ((dx <= dy) && (dx <= dz))
                d = dx;
            else if (dy <= dz)
                d = dy;
            else
                d = dz;
        } else
            d = resolution;

        cout << "Reconstructed volume voxel size : " << d << " mm" << endl;

        RealPixel smin, smax;
        stack.GetMinMax(&smin, &smax);
        enlarged.Initialize(stack.Attributes());

        // interpolate the input stack to the given resolution
        if (smin < -0.1) {
            GenericLinearInterpolateImageFunction<RealImage> interpolator;
            ResamplingWithPadding<RealPixel> resampler(d, d, d, -1);
            resampler.Input(&stack);
            resampler.Output(&enlarged);
            resampler.Interpolator(&interpolator);
            resampler.Run();
        } else {
            if (smin < 0.1) {
                GenericLinearInterpolateImageFunction<RealImage> interpolator;
                ResamplingWithPadding<RealPixel> resampler(d, d, d, 0);
                resampler.Input(&stack);
                resampler.Output(&enlarged);
                resampler.Interpolator(&interpolator);
                resampler.Run();
            } else {
                //resample "enlarged" to resolution "d"
                unique_ptr<InterpolateImageFunction> interpolator(InterpolateImageFunction::New(Interpolation_Linear));
                Resampling<RealPixel> resampler(d, d, d);
                resampler.Input(&stack);
                resampler.Output(&enlarged);
                resampler.Interpolator(interpolator.get());
                resampler.Run();
            }
        }

        //initialise reconstructed volume
        _reconstructed = move(enlarged);
        _template_created = true;

        if (_debug)
            _reconstructed.Write("template.nii.gz");
        _grey_reconstructed = _reconstructed;
        _attr_reconstructed = _reconstructed.Attributes();

        //return resulting resolution of the template image
        return d;
    }

    //-------------------------------------------------------------------

    // Create anisotropic template
    double Reconstruction::CreateTemplateAniso(const RealImage& stack) {
        //Get image attributes - image size and voxel size
        ImageAttributes attr = stack.Attributes();

        //enlarge stack in z-direction in case top of the head is cut off
        attr._t = 1;

        //create enlarged image
        RealImage enlarged(attr);

        cout << "Constructing volume with anisotropic voxel size " << attr._x << " " << attr._y << " " << attr._z << endl;

        //initialize reconstructed volume
        _reconstructed = move(enlarged);
        _template_created = true;

        //return resulting resolution of the template image
        return attr._x;
    }

    //-------------------------------------------------------------------

    // Set template
    void Reconstruction::SetTemplate(RealImage templateImage) {
        RealImage t2template(_reconstructed.Attributes());
        RigidTransformation tr;
        GenericLinearInterpolateImageFunction<RealImage> interpolator;
        ImageTransformation imagetransformation;

        imagetransformation.Input(&templateImage);
        imagetransformation.Transformation(&tr);
        imagetransformation.Output(&t2template);
        //target contains zeros, need padding -1
        imagetransformation.TargetPaddingValue(-1);
        //need to fill voxels in target where there is no info from source with zeroes
        imagetransformation.SourcePaddingValue(0);
        imagetransformation.Interpolator(&interpolator);
        imagetransformation.Run();

        _reconstructed = move(t2template);
    }

    //-------------------------------------------------------------------

    // binarise mask
    RealImage Reconstruction::CreateMask(RealImage image) {
        RealPixel *ptr = image.Data();
        for (int i = 0; i < image.NumberOfVoxels(); i++)
            ptr[i] = ptr[i] > 0.5 ? 1 : 0;

        return image;
    }

    //-------------------------------------------------------------------

    // normalise and threshold mask
    RealImage Reconstruction::ThresholdNormalisedMask(RealImage image, double threshold) {
        RealPixel smin, smax;
        image.GetMinMax(&smin, &smax);

        if (smax > 0)
            image /= smax;

        RealPixel *ptr = image.Data();
        for (int i = 0; i < image.NumberOfVoxels(); i++)
            ptr[i] = ptr[i] > threshold ? 1 : 0;

        return image;
    }

    //-------------------------------------------------------------------

    // create mask from dark / black background
    void Reconstruction::CreateMaskFromBlackBackground(const Array<RealImage>& stacks, Array<RigidTransformation> stack_transformations, double smooth_mask) {
        //Create average of the stack using currect stack transformations
        GreyImage average = CreateAverage(stacks, stack_transformations);

        GreyPixel *ptr = average.Data();
        #pragma omp parallel for
        for (int i = 0; i < average.NumberOfVoxels(); i++)
            if (ptr[i] < 0)
                ptr[i] = 0;

        //Create mask of the average from the black background
        MeanShift msh(average, 0, 256);
        msh.GenerateDensity();
        msh.SetThreshold();
        msh.RemoveBackground();
        GreyImage mask = msh.ReturnMask();

        //Calculate LCC of the mask to remove disconnected structures
        MeanShift msh2(mask, 0, 256);
        msh2.SetOutput(&mask);
        msh2.Lcc(1);
    }

    //-------------------------------------------------------------------

    // Set given mask to the reconstruction object
    void Reconstruction::SetMask(RealImage *mask, double sigma, double threshold) {
        if (!_template_created) {
            cerr << "Please create the template before setting the mask, so that the mask can be resampled to the correct dimensions." << endl;
            exit(1);
        }

        _mask = _reconstructed;

        if (mask != NULL) {
            //if sigma is nonzero first smooth the mask
            if (sigma > 0) {
                //blur mask
                GaussianBlurring<RealPixel> gb(sigma);

                gb.Input(mask);
                gb.Output(mask);
                gb.Run();

                //binarise mask
                *mask = CreateMask(*mask, threshold);
            }

            //resample the mask according to the template volume using identity transformation
            RigidTransformation transformation;
            ImageTransformation imagetransformation;

            //GenericNearestNeighborExtrapolateImageFunction<RealImage> interpolator;
            InterpolationMode interpolation = Interpolation_NN;
            unique_ptr<InterpolateImageFunction> interpolator(InterpolateImageFunction::New(interpolation));

            imagetransformation.Input(mask);
            imagetransformation.Transformation(&transformation);
            imagetransformation.Output(&_mask);
            //target is zero image, need padding -1
            imagetransformation.TargetPaddingValue(-1);
            //need to fill voxels in target where there is no info from source with zeroes
            imagetransformation.SourcePaddingValue(0);
            imagetransformation.Interpolator(interpolator.get());
            imagetransformation.Run();
        } else {
            //fill the mask with ones
            _mask = 1;
        }
        //set flag that mask was created
        _have_mask = true;

        // compute mask volume
        double vol = 0;
        RealPixel *pm = _mask.Data();
        #pragma omp parallel for reduction(+: vol)
        for (int i = 0; i < _mask.NumberOfVoxels(); i++)
            if (pm[i] > 0.1)
                vol++;

        vol *= _reconstructed.GetXSize() * _reconstructed.GetYSize() * _reconstructed.GetZSize() / 1000;

        cout << "ROI volume : " << vol << " cc " << endl;

        if (_debug)
            _mask.Write("mask.nii.gz");
    }

    //-------------------------------------------------------------------

    // transform mask to the specified stack space
    void Reconstruction::TransformMask(const RealImage& image, RealImage& mask, const RigidTransformation& transformation) {
        //transform mask to the space of image
        unique_ptr<InterpolateImageFunction> interpolator(InterpolateImageFunction::New(Interpolation_NN));
        ImageTransformation imagetransformation;
        RealImage m(image.Attributes());

        imagetransformation.Input(&mask);
        imagetransformation.Transformation(&transformation);
        imagetransformation.Output(&m);
        //target contains zeros and ones image, need padding -1
        imagetransformation.TargetPaddingValue(-1);
        //need to fill voxels in target where there is no info from source with zeroes
        imagetransformation.SourcePaddingValue(0);
        imagetransformation.Interpolator(interpolator.get());
        imagetransformation.Run();
        mask = move(m);
    }

    //-------------------------------------------------------------------

    // reset image origin and save it into the output transforamtion (GreyImage)
    void Reconstruction::ResetOrigin(GreyImage& image, RigidTransformation& transformation) {
        double ox, oy, oz;
        image.GetOrigin(ox, oy, oz);
        image.PutOrigin(0, 0, 0);
        transformation.PutTranslationX(ox);
        transformation.PutTranslationY(oy);
        transformation.PutTranslationZ(oz);
        transformation.PutRotationX(0);
        transformation.PutRotationY(0);
        transformation.PutRotationZ(0);
    }

    //-------------------------------------------------------------------

    // reset image origin and save it into the output transforamtion (RealImage)
    void Reconstruction::ResetOrigin(RealImage& image, RigidTransformation& transformation) {
        double ox, oy, oz;
        image.GetOrigin(ox, oy, oz);
        image.PutOrigin(0, 0, 0);
        transformation.PutTranslationX(ox);
        transformation.PutTranslationY(oy);
        transformation.PutTranslationZ(oz);
        transformation.PutRotationX(0);
        transformation.PutRotationY(0);
        transformation.PutRotationZ(0);
    }

    //-------------------------------------------------------------------

    // generate reconstruction quality report / metrics
    void Reconstruction::ReconQualityReport(double& out_ncc, double& out_nrmse, double& average_weight, double& ratio_excluded) {
        Parallel::QualityReport parallelQualityReport(this);
        parallelQualityReport();

        average_weight = _average_volume_weight;
        out_ncc = parallelQualityReport.out_global_ncc / _slices.size();
        out_nrmse = parallelQualityReport.out_global_nrmse / _slices.size();

        if (!isfinite(out_nrmse))
            out_nrmse = 0;

        if (!isfinite(out_ncc))
            out_ncc = 0;

        size_t count_excluded = 0;
        for (size_t i = 0; i < _slices.size(); i++)
            if (_slice_weight[i] < 0.5)
                count_excluded++;

        ratio_excluded = (double)count_excluded / _slices.size();
    }

    //-------------------------------------------------------------------

    // compute inter-slice volume NCC (motion metric)
    double Reconstruction::VolumeNCC(RealImage& input_stack, RealImage template_stack, const RealImage& mask) {
        template_stack *= mask;

        RigidTransformation r_init;
        r_init.PutTranslationX(0.0001);
        r_init.PutTranslationY(0.0001);
        r_init.PutTranslationZ(-0.0001);

        ParameterList params;
        Insert(params, "Transformation model", "Rigid");
        Insert(params, "Background value for image 1", 0);

        GenericRegistrationFilter registration;
        registration.Parameter(params);
        registration.Input(&template_stack, &input_stack);
        Transformation *dofout = nullptr;
        registration.Output(&dofout);
        registration.InitialGuess(&r_init);
        registration.GuessParameter();
        registration.Run();
        unique_ptr<RigidTransformation> r_dofout(dynamic_cast<RigidTransformation*>(dofout));

        GenericLinearInterpolateImageFunction<RealImage> interpolator;
        double source_padding = 0;
        double target_padding = -inf;
        bool dofin_invert = false;
        bool twod = false;

        RealImage& output = template_stack;
        memset(output.Data(), 0, sizeof(RealPixel) * output.NumberOfVoxels());

        ImageTransformation imagetransformation;
        imagetransformation.Input(&input_stack);
        imagetransformation.Transformation(r_dofout.get());
        imagetransformation.Output(&output);
        imagetransformation.TargetPaddingValue(target_padding);
        imagetransformation.SourcePaddingValue(source_padding);
        imagetransformation.Interpolator(&interpolator);
        imagetransformation.TwoD(twod);
        imagetransformation.Invert(dofin_invert);
        imagetransformation.Run();

        input_stack = output * mask;

        double ncc = 0;
        int count = 0;
        for (int z = 0; z < input_stack.GetZ() - 1; z++) {
            constexpr int sh = 5;
            const RealImage slice_1 = input_stack.GetRegion(sh, sh, z, input_stack.GetX() - sh, input_stack.GetY() - sh, z + 1);
            const RealImage slice_2 = input_stack.GetRegion(sh, sh, z + 1, input_stack.GetX() - sh, input_stack.GetY() - sh, z + 2);

            double slice_count = -1;
            const double slice_ncc = ComputeNCC(slice_1, slice_2, 0.1, &slice_count);
            if (slice_ncc > 0) {
                ncc += slice_ncc;
                count++;
            }
        }

        return ncc / count;
    }

    //-------------------------------------------------------------------

    // run global stack registration to the template
    void Reconstruction::StackRegistrations(const Array<RealImage>& stacks, Array<RigidTransformation>& stack_transformations, int templateNumber) {
        SVRTK_START_TIMING();

        InvertStackTransformations(stack_transformations);

        // check whether to use the global template or the selected stack
        RealImage target = _template_flag ? _reconstructed : stacks[templateNumber];

        RealImage m_tmp = _mask;
        TransformMask(target, m_tmp, RigidTransformation());
        target *= m_tmp;
        target.Write("masked.nii.gz");

        if (_debug) {
            target.Write("target.nii.gz");
            stacks[0].Write("stack0.nii.gz");
        }

        RigidTransformation offset;
        ResetOrigin(target, offset);

        //register all stacks to the target
        Parallel::StackRegistrations p_reg(this, stacks, stack_transformations, templateNumber, target, offset);
        p_reg();

        InvertStackTransformations(stack_transformations);

        SVRTK_END_TIMING("StackRegistrations");
    }

    //-------------------------------------------------------------------

    // restore the original slice intensities
    void Reconstruction::RestoreSliceIntensities() {
        #pragma omp parallel for
        for (size_t inputIndex = 0; inputIndex < _slices.size(); inputIndex++) {
            //calculate scaling factor
            const double& factor = _stack_factor[_stack_index[inputIndex]];//_average_value;

            // read the pointer to current slice
            RealPixel *p = _slices[inputIndex].Data();
            for (int i = 0; i < _slices[inputIndex].NumberOfVoxels(); i++)
                if (p[i] > 0)
                    p[i] /= factor;
        }
    }

    //-------------------------------------------------------------------

    // scale the reconstructed volume
    void Reconstruction::ScaleVolume(RealImage& reconstructed) {
        double scalenum = 0, scaleden = 0;

        #pragma omp parallel for
        for (size_t inputIndex = 0; inputIndex < _slices.size(); inputIndex++) {
            // alias for the current slice
            const RealImage& slice = _slices[inputIndex];

            //alias for the current weight image
            const RealImage& w = _weights[inputIndex];

            // alias for the current simulated slice
            const RealImage& sim = _simulated_slices[inputIndex];

            for (int i = 0; i < slice.GetX(); i++)
                for (int j = 0; j < slice.GetY(); j++)
                    if (slice(i, j, 0) != -1) {
                        //scale - intensity matching
                        if (_simulated_weights[inputIndex](i, j, 0) > 0.99) {
                            scalenum += w(i, j, 0) * _slice_weight[inputIndex] * slice(i, j, 0) * sim(i, j, 0);
                            scaleden += w(i, j, 0) * _slice_weight[inputIndex] * sim(i, j, 0) * sim(i, j, 0);
                        }
                    }

        } //end of loop for a slice inputIndex

        //calculate scale for the volume
        double scale = 1;

        if (scaleden > 0)
            scale = scalenum / scaleden;

        if (_verbose)
            _verbose_log << "scale : " << scale << endl;

        RealPixel *ptr = reconstructed.Data();
        #pragma omp parallel for
        for (int i = 0; i < reconstructed.NumberOfVoxels(); i++)
            if (ptr[i] > 0)
                ptr[i] *= scale;
        }
    }

    //-------------------------------------------------------------------

    void Reconstruction::ScaleVolume() {
        ScaleVolume(_reconstructed);
    }

    //-------------------------------------------------------------------

    // run simulation of slices from the reconstruction volume
    void Reconstruction::SimulateSlices() {
        SVRTK_START_TIMING();
        Parallel::SimulateSlices p_sim(this);
        p_sim();
        SVRTK_END_TIMING("SimulateSlices");
    }

    //-------------------------------------------------------------------

    // simulate stacks from the reconstructed volume
    void Reconstruction::SimulateStacks(Array<RealImage>& stacks) {
        RealImage sim;
        int z = -1;//this is the z coordinate of the stack
        int current_stack = -1; //we need to know when to start a new stack

        for (size_t inputIndex = 0; inputIndex < _slices.size(); inputIndex++) {
            // read the current slice
            const RealImage& slice = _slices[inputIndex];

            //Calculate simulated slice
            sim.Initialize(slice.Attributes());

            //do not simulate excluded slice
            if (_slice_weight[inputIndex] > 0.5) {
                #pragma omp parallel for
                for (int i = 0; i < slice.GetX(); i++)
                    for (int j = 0; j < slice.GetY(); j++)
                        if (slice(i, j, 0) > -0.01) {
                            double weight = 0;
                            for (size_t k = 0; k < _volcoeffs[inputIndex][i][j].size(); k++) {
                                const POINT3D& p = _volcoeffs[inputIndex][i][j][k];
                                sim(i, j, 0) += p.value * _reconstructed(p.x, p.y, p.z);
                                weight += p.value;
                            }
                            if (weight > 0.98)
                                sim(i, j, 0) /= weight;
                            else
                                sim(i, j, 0) = 0;
                        }
            }

            if (_stack_index[inputIndex] == current_stack)
                z++;
            else {
                current_stack = _stack_index[inputIndex];
                z = 0;
            }

            #pragma omp parallel for
            for (int i = 0; i < sim.GetX(); i++)
                for (int j = 0; j < sim.GetY(); j++)
                    stacks[_stack_index[inputIndex]](i, j, z) = sim(i, j, 0);
        }
    }

    //-------------------------------------------------------------------

    // match stack intensities with respect to the average
    void Reconstruction::MatchStackIntensities(Array<RealImage>& stacks, const Array<RigidTransformation>& stack_transformations, double averageValue, bool together) {
        //Calculate the averages of intensities for all stacks
        Array<double> stack_average;
        stack_average.reserve(stacks.size());

        //remember the set average value
        _average_value = averageValue;

        //averages need to be calculated only in ROI
        for (size_t ind = 0; ind < stacks.size(); ind++) {
            double sum = 0, num = 0;
            #pragma omp parallel for reduction(+: sum, num)
            for (int i = 0; i < stacks[ind].GetX(); i++)
                for (int j = 0; j < stacks[ind].GetY(); j++)
                    for (int k = 0; k < stacks[ind].GetZ(); k++) {
                        //image coordinates of the stack voxel
                        double x = i;
                        double y = j;
                        double z = k;
                        //change to world coordinates
                        stacks[ind].ImageToWorld(x, y, z);
                        //transform to template (and also _mask) space
                        stack_transformations[ind].Transform(x, y, z);
                        //change to mask image coordinates - mask is aligned with template
                        _mask.WorldToImage(x, y, z);
                        x = round(x);
                        y = round(y);
                        z = round(z);
                        //if the voxel is inside mask ROI include it
                        if (stacks[ind](i, j, k) > 0) {
                            sum += stacks[ind](i, j, k);
                            num++;
                        }
                    }
            //calculate average for the stack
            if (num > 0) {
                stack_average.push_back(sum / num);
            } else {
                cerr << "Stack " << ind << " has no overlap with ROI" << endl;
                exit(1);
            }
        }

        double global_average = 0;
        if (together) {
            for (size_t i = 0; i < stack_average.size(); i++)
                global_average += stack_average[i];
            global_average /= stack_average.size();
        }

        if (_verbose) {
            _verbose_log << "Stack average intensities are ";
            for (size_t ind = 0; ind < stack_average.size(); ind++)
                _verbose_log << stack_average[ind] << " ";
            _verbose_log << endl;
            _verbose_log << "The new average value is " << averageValue << endl;
        }

        //Rescale stacks
        ClearAndReserve(_stack_factor, stacks.size());
        for (size_t ind = 0; ind < stacks.size(); ind++) {
            const double factor = averageValue / (together ? global_average : stack_average[ind]);
            _stack_factor.push_back(factor);

            RealPixel *ptr = stacks[ind].Data();
            #pragma omp parallel for
            for (int i = 0; i < stacks[ind].NumberOfVoxels(); i++)
                if (ptr[i] > 0)
                    ptr[i] *= factor;
        }

        if (_debug) {
            #pragma omp parallel for
            for (size_t ind = 0; ind < stacks.size(); ind++)
                stacks[ind].Write((boost::format("rescaled-stack%1%.nii.gz") % ind).str().c_str());
        }

        if (_verbose) {
            _verbose_log << "Slice intensity factors are ";
            for (size_t ind = 0; ind < _stack_factor.size(); ind++)
                _verbose_log << _stack_factor[ind] << " ";
            _verbose_log << endl;
            _verbose_log << "The new average value is " << averageValue << endl;
        }
    }

    //-------------------------------------------------------------------

    // evaluate reconstruction quality (NRMSE)
    double Reconstruction::EvaluateReconQuality(int stackIndex) {
        Array<double> rmse_values(_slices.size());
        Array<int> rmse_numbers(_slices.size());
        RealImage nt;

        // compute NRMSE between the simulated and original slice for non-zero voxels
        #pragma omp parallel for private(nt)
        for (size_t inputIndex = 0; inputIndex < _slices.size(); inputIndex++) {
            nt = _slices[inputIndex];
            const RealImage& ns = _simulated_slices[inputIndex];
            double s_diff = 0;
            double s_t = 0;
            int s_n = 0;
            for (int x = 0; x < nt.GetX(); x++) {
                for (int y = 0; y < nt.GetY(); y++) {
                    if (nt(x, y, 0) > 0 && ns(x, y, 0) > 0) {
                        nt(x, y, 0) *= exp(-(_bias[inputIndex])(x, y, 0)) * _scale[inputIndex];
                        s_t += nt(x, y, 0);
                        s_diff += (nt(x, y, 0) - ns(x, y, 0)) * (nt(x, y, 0) - ns(x, y, 0));
                        s_n++;
                    }
                }
            }
            const double nrmse = s_n > 0 ? sqrt(s_diff / s_n) / (s_t / s_n) : 0;
            rmse_values[inputIndex] = nrmse;
            if (nrmse > 0)
                rmse_numbers[inputIndex] = 1;
        }

        double rmse_total = 0;
        int slice_n = 0;
        #pragma omp simd
        for (size_t inputIndex = 0; inputIndex < _slices.size(); inputIndex++) {
            rmse_total += rmse_values[inputIndex];
            slice_n += rmse_numbers[inputIndex];
        }

        if (slice_n > 0)
            rmse_total /= slice_n;
        else
            rmse_total = 0;

        return rmse_total;
    }

    //-------------------------------------------------------------------

    // mask stacks with respect to the reconstruction mask and given transformations
    void Reconstruction::MaskStacks(Array<RealImage>& stacks, Array<RigidTransformation>& stack_transformations) {
        //Check whether we have a mask
        if (!_have_mask) {
            cerr << "Could not mask slices because no mask has been set." << endl;
            return;
        }

        for (size_t inputIndex = 0; inputIndex < stacks.size(); inputIndex++) {
            RealImage& stack = stacks[inputIndex];
            #pragma omp parallel for
            for (int i = 0; i < stack.GetX(); i++)
                for (int j = 0; j < stack.GetY(); j++) {
                    for (int k = 0; k < stack.GetZ(); k++) {
                        //if the value is smaller than 1 assume it is padding
                        if (stack(i, j, k) < 0.01)
                            stack(i, j, k) = -1;
                        //image coordinates of a slice voxel
                        double x = i;
                        double y = j;
                        double z = k;
                        //change to world coordinates in slice space
                        stack.ImageToWorld(x, y, z);
                        //world coordinates in volume space
                        stack_transformations[inputIndex].Transform(x, y, z);
                        //image coordinates in volume space
                        _mask.WorldToImage(x, y, z);
                        x = round(x);
                        y = round(y);
                        z = round(z);
                        //if the voxel is outside mask ROI set it to -1 (padding value)
                        if ((x >= 0) && (x < _mask.GetX()) && (y >= 0) && (y < _mask.GetY()) && (z >= 0) && (z < _mask.GetZ())) {
                            if (_mask(x, y, z) == 0)
                                stack(i, j, k) = -1;
                        } else
                            stack(i, j, k) = -1;
                    }
                }
        }
    }

    //-------------------------------------------------------------------

    // match stack intensities with respect to the masked ROI
    void Reconstruction::MatchStackIntensitiesWithMasking(Array<RealImage>& stacks, const Array<RigidTransformation>& stack_transformations, double averageValue, bool together) {
        SVRTK_START_TIMING();

        RealImage m;
        Array<double> stack_average;
        stack_average.reserve(stacks.size());

        //remember the set average value
        _average_value = averageValue;

        //Calculate the averages of intensities for all stacks in the mask ROI
        for (size_t ind = 0; ind < stacks.size(); ind++) {
            double sum = 0, num = 0;

            if (_debug)
                m = stacks[ind];

            #pragma omp parallel for reduction(+: sum, num)
            for (int i = 0; i < stacks[ind].GetX(); i++)
                for (int j = 0; j < stacks[ind].GetY(); j++)
                    for (int k = 0; k < stacks[ind].GetZ(); k++) {
                        //image coordinates of the stack voxel
                        double x = i;
                        double y = j;
                        double z = k;
                        //change to world coordinates
                        stacks[ind].ImageToWorld(x, y, z);
                        //transform to template (and also _mask) space
                        stack_transformations[ind].Transform(x, y, z);
                        //change to mask image coordinates - mask is aligned with template
                        _mask.WorldToImage(x, y, z);
                        x = round(x);
                        y = round(y);
                        z = round(z);
                        //if the voxel is inside mask ROI include it
                        if (x >= 0 && x < _mask.GetX() && y >= 0 && y < _mask.GetY() && z >= 0 && z < _mask.GetZ()) {
                            if (_mask(x, y, z) == 1) {
                                if (_debug)
                                    m(i, j, k) = 1;
                                sum += stacks[ind](i, j, k);
                                num++;
                            } else if (_debug)
                                m(i, j, k) = 0;
                        }
                    }
            if (_debug)
                m.Write((boost::format("mask-for-matching%1%.nii.gz") % ind).str().c_str());

            //calculate average for the stack
            if (num > 0) {
                stack_average.push_back(sum / num);
            } else {
                cerr << "Stack " << ind << " has no overlap with ROI" << endl;
                exit(1);
            }
        }

        double global_average = 0;
        if (together) {
            for (size_t i = 0; i < stack_average.size(); i++)
                global_average += stack_average[i];
            global_average /= stack_average.size();
        }

        if (_verbose) {
            _verbose_log << "Stack average intensities are ";
            for (size_t ind = 0; ind < stack_average.size(); ind++)
                _verbose_log << stack_average[ind] << " ";
            _verbose_log << endl;
            _verbose_log << "The new average value is " << averageValue << endl;
        }

        //Rescale stacks
        ClearAndReserve(_stack_factor, stacks.size());
        for (size_t ind = 0; ind < stacks.size(); ind++) {
            double factor = averageValue / (together ? global_average : stack_average[ind]);
            _stack_factor.push_back(factor);

            RealPixel *ptr = stacks[ind].Data();
            #pragma omp parallel for
            for (int i = 0; i < stacks[ind].NumberOfVoxels(); i++)
                if (ptr[i] > 0)
                    ptr[i] *= factor;
        }

        if (_debug) {
            #pragma omp parallel for
            for (size_t ind = 0; ind < stacks.size(); ind++)
                stacks[ind].Write((boost::format("rescaled-stack%1%.nii.gz") % ind).str().c_str());
        }

        if (_verbose) {
            _verbose_log << "Slice intensity factors are ";
            for (size_t ind = 0; ind < stack_average.size(); ind++)
                _verbose_log << _stack_factor[ind] << " ";
            _verbose_log << endl;
            _verbose_log << "The new average value is " << averageValue << endl;
        }

        SVRTK_END_TIMING("MatchStackIntensitiesWithMasking");
    }

    //-------------------------------------------------------------------

    // create slices from the input stacks
    void Reconstruction::CreateSlicesAndTransformations(const Array<RealImage>& stacks, const Array<RigidTransformation>& stack_transformations, const Array<double>& thickness, const Array<RealImage>& probability_maps) {
        double average_thickness = 0;

        // Reset and allocate memory
        const size_t reserve_size = stacks.size() * stacks[0].Attributes()._z;
        ClearAndReserve(_zero_slices, reserve_size);
        ClearAndReserve(_slices, reserve_size);
        ClearAndReserve(_package_index, reserve_size);
        ClearAndReserve(_slice_attributes, reserve_size);
        ClearAndReserve(_grey_slices, reserve_size);
        ClearAndReserve(_slice_dif, reserve_size);
        ClearAndReserve(_simulated_slices, reserve_size);
        ClearAndReserve(_reg_slice_weight, reserve_size);
        ClearAndReserve(_slice_pos, reserve_size);
        ClearAndReserve(_simulated_weights, reserve_size);
        ClearAndReserve(_simulated_inside, reserve_size);
        ClearAndReserve(_stack_index, reserve_size);
        ClearAndReserve(_transformations, reserve_size);
        if (_ffd)
            ClearAndReserve(_mffd_transformations, reserve_size);
        if (!probability_maps.empty())
            ClearAndReserve(_probability_maps, reserve_size);

        //for each stack
        for (size_t i = 0; i < stacks.size(); i++) {
            //image attributes contain image and voxel size
            const ImageAttributes& attr = stacks[i].Attributes();

            int package_index = 0;
            int current_package = -1;

            //attr._z is number of slices in the stack
            for (int j = 0; j < attr._z; j++) {
                if (!_n_packages.empty()) {
                    current_package++;
                    if (current_package > _n_packages[i] - 1)
                        current_package = 0;
                } else {
                    current_package = 0;
                }

                bool excluded = false;
                for (size_t fe = 0; fe < _excluded_entirely.size(); fe++) {
                    if (j == _excluded_entirely[fe]) {
                        excluded = true;
                        break;
                    }
                }

                if (excluded)
                    continue;

                //create slice by selecting the appropriate region of the stack
                RealImage slice = stacks[i].GetRegion(0, 0, j, attr._x, attr._y, j + 1);
                //set correct voxel size in the stack. Z size is equal to slice thickness.
                slice.PutPixelSize(attr._dx, attr._dy, thickness[i]);
                //remember the slice
                RealPixel tmin, tmax;
                slice.GetMinMax(&tmin, &tmax);
                _zero_slices.push_back(tmax > 1 && (tmax - tmin) > 1 ? 1 : -1);

                // if 2D gaussian filtering is required
                if (_blurring) {
                    GaussianBlurring<RealPixel> gbt(0.6 * slice.GetXSize());
                    gbt.Input(&slice);
                    gbt.Output(&slice);
                    gbt.Run();
                }

                _slices.push_back(slice);
                _package_index.push_back(current_package);
                _slice_attributes.push_back(slice.Attributes());

                _grey_slices.push_back(slice);
                memset(slice.Data(), 0, sizeof(RealPixel) * slice.NumberOfVoxels());
                _slice_dif.push_back(slice);
                _simulated_slices.push_back(slice);
                _reg_slice_weight.push_back(1);
                _slice_pos.push_back(j);
                slice = 1;
                _simulated_weights.push_back(slice);
                _simulated_inside.push_back(move(slice));
                //remember stack index for this slice
                _stack_index.push_back(i);
                //initialize slice transformation with the stack transformation
                _transformations.push_back(stack_transformations[i]);

                // if non-rigid FFD registartion option was selected
                if (_ffd)
                    _mffd_transformations.push_back(MultiLevelFreeFormTransformation());

                if (!probability_maps.empty()) {
                    RealImage proba = probability_maps[i].GetRegion(0, 0, j, attr._x, attr._y, j + 1);
                    proba.PutPixelSize(attr._dx, attr._dy, thickness[i]);
                    _probability_maps.push_back(move(proba));
                }

                average_thickness += thickness[i];
            }
        }
        cout << "Number of slices: " << _slices.size() << endl;
        _number_of_slices_org = _slices.size();
        _average_thickness_org = average_thickness / _number_of_slices_org;
    }

    //-------------------------------------------------------------------

    // reset slices array and read them from the input stacks
    void Reconstruction::ResetSlices(Array<RealImage>& stacks, Array<double>& thickness) {
        if (_verbose)
            _verbose_log << "ResetSlices" << endl;

        UpdateSlices(stacks, thickness);

        #pragma omp parallel for
        for (size_t i = 0; i < _slices.size(); i++) {
            _bias[i].Initialize(_slices[i].Attributes());
            _weights[i].Initialize(_slices[i].Attributes());
        }
    }

    //-------------------------------------------------------------------

    // set slices and transformation from the given array
    void Reconstruction::SetSlicesAndTransformations(const Array<RealImage>& slices, const Array<RigidTransformation>& slice_transformations, const Array<int>& stack_ids, const Array<double>& thickness) {
        ClearAndReserve(_slices, slices.size());
        ClearAndReserve(_stack_index, slices.size());
        ClearAndReserve(_transformations, slices.size());

        //for each slice
        for (size_t i = 0; i < slices.size(); i++) {
            //get slice
            RealImage slice = slices[i];
            cout << "setting slice " << i << "\n";
            slice.Print();
            //set correct voxel size in the stack. Z size is equal to slice thickness.
            slice.PutPixelSize(slice.GetXSize(), slice.GetYSize(), thickness[i]);
            //remember the slice
            _slices.push_back(move(slice));
            //remember stack index for this slice
            _stack_index.push_back(stack_ids[i]);
            //get slice transformation
            _transformations.push_back(slice_transformations[i]);
        }
    }

    //-------------------------------------------------------------------

    // update slices array based on the given stacks
    void Reconstruction::UpdateSlices(Array<RealImage>& stacks, Array<double>& thickness) {
        ClearAndReserve(_slices, stacks.size() * stacks[0].Attributes()._z);

        //for each stack
        for (size_t i = 0; i < stacks.size(); i++) {
            //image attributes contain image and voxel size
            const ImageAttributes& attr = stacks[i].Attributes();

            //attr._z is number of slices in the stack
            for (int j = 0; j < attr._z; j++) {
                //create slice by selecting the appropriate region of the stack
                RealImage slice = stacks[i].GetRegion(0, 0, j, attr._x, attr._y, j + 1);
                //set correct voxel size in the stack. Z size is equal to slice thickness.
                slice.PutPixelSize(attr._dx, attr._dy, thickness[i]);
                //remember the slice
                _slices.push_back(move(slice));
            }
        }
        cout << "Number of slices: " << _slices.size() << endl;
    }

    //-------------------------------------------------------------------

    // mask slices based on the reconstruction mask
    void Reconstruction::MaskSlices() {
        //Check whether we have a mask
        if (!_have_mask) {
            cerr << "Could not mask slices because no mask has been set." << endl;
            return;
        }

        //mask slices
        #pragma omp parallel for
        for (size_t inputIndex = 0; inputIndex < _slices.size(); inputIndex++) {
            RealImage& slice = _slices[inputIndex];
            for (int i = 0; i < slice.GetX(); i++)
                for (int j = 0; j < slice.GetY(); j++) {
                    //if the value is smaller than 1 assume it is padding
                    if (slice(i, j, 0) < 0.01)
                        slice(i, j, 0) = -1;
                    //image coordinates of a slice voxel
                    double x = i;
                    double y = j;
                    double z = 0;
                    //change to world coordinates in slice space
                    slice.ImageToWorld(x, y, z);

                    // use either rigid or FFD transformation models
                    if (!_ffd)
                        _transformations[inputIndex].Transform(x, y, z);
                    else
                        _mffd_transformations[inputIndex].Transform(-1, 1, x, y, z);

                    //image coordinates in volume space
                    _mask.WorldToImage(x, y, z);
                    x = round(x);
                    y = round(y);
                    z = round(z);
                    //if the voxel is outside mask ROI set it to -1 (padding value)
                    if ((x >= 0) && (x < _mask.GetX()) && (y >= 0) && (y < _mask.GetY()) && (z >= 0) && (z < _mask.GetZ())) {
                        if (_mask(x, y, z) == 0)
                            slice(i, j, 0) = -1;
                    } else
                        slice(i, j, 0) = -1;
                }
        }
    }

    //-------------------------------------------------------------------

    // transform slice coordinates to the reconstructed space
    void Reconstruction::Transform2Reconstructed(const int inputIndex, int& i, int& j, int& k, const int mode) {
        double x = i;
        double y = j;
        double z = k;

        _slices[inputIndex].ImageToWorld(x, y, z);

        if (!_ffd)
            _transformations[inputIndex].Transform(x, y, z);
        else
            _mffd_transformations[inputIndex].Transform(-1, 1, x, y, z);

        _reconstructed.WorldToImage(x, y, z);

        if (mode == 0) {
            i = (int)round(x);
            j = (int)round(y);
            k = (int)round(z);
        } else {
            i = (int)floor(x);
            j = (int)floor(y);
            k = (int)floor(z);
        }
    }

    //-------------------------------------------------------------------

    // initialise slice transfromations with stack transformations
    void Reconstruction::InitialiseWithStackTransformations(const Array<RigidTransformation>& stack_transformations) {
        #pragma omp parallel for
        for (size_t sliceIndex = 0; sliceIndex < _slices.size(); sliceIndex++) {
            const auto& stack_transformation = stack_transformations[_stack_index[sliceIndex]];
            auto& transformation = _transformations[sliceIndex];
            transformation.PutTranslationX(stack_transformation.GetTranslationX());
            transformation.PutTranslationY(stack_transformation.GetTranslationY());
            transformation.PutTranslationZ(stack_transformation.GetTranslationZ());
            transformation.PutRotationX(stack_transformation.GetRotationX());
            transformation.PutRotationY(stack_transformation.GetRotationY());
            transformation.PutRotationZ(stack_transformation.GetRotationZ());
            transformation.UpdateMatrix();
        }
    }

    //-------------------------------------------------------------------

    // structure-based outlier rejection step (NCC-based)
    void Reconstruction::StructuralExclusion() {
        SVRTK_START_TIMING();

        double source_padding = -1;
        const double target_padding = -inf;
        constexpr bool dofin_invert = false;
        constexpr bool twod = false;

        const RealImage& source = _reconstructed;
        RealPixel smin, smax;
        source.GetMinMax(&smin, &smax);

        if (smin < -0.1)
            source_padding = -1;
        else if (smin < 0.1)
            source_padding = 0;

        Array<double> reg_ncc(_slices.size());
        double mean_ncc = 0;

        cout << " - excluded : ";

        RealImage output, target, slice_mask;
        GenericLinearInterpolateImageFunction<RealImage> interpolator;
        ImageTransformation imagetransformation;
        imagetransformation.Input(&source);
        imagetransformation.Output(&output);
        imagetransformation.TargetPaddingValue(target_padding);
        imagetransformation.SourcePaddingValue(source_padding);
        imagetransformation.Interpolator(&interpolator);
        imagetransformation.TwoD(twod);
        imagetransformation.Invert(dofin_invert);

        #pragma omp parallel for private(output, target, slice_mask) firstprivate(imagetransformation) reduction(+: mean_ncc)
        for (size_t inputIndex = 0; inputIndex < _slices.size(); inputIndex++) {
            // transfrom reconstructed volume to the slice space
            imagetransformation.Transformation(&_transformations[inputIndex]);
            imagetransformation.Run();

            // blur the original slice
            target.Initialize(_slices[inputIndex].Attributes());
            GaussianBlurringWithPadding<RealPixel> gb(target.GetXSize() * 0.6, source_padding);
            gb.Input(&_slices[inputIndex]);
            gb.Output(&target);
            gb.Run();

            // mask slices
            slice_mask = _mask;
            TransformMask(target, slice_mask, _transformations[inputIndex]);
            target *= slice_mask;
            output.Initialize(_slices[inputIndex].Attributes());
            output *= slice_mask;

            // compute NCC
            double output_ncc = ComputeNCC(target, output, 0);
            if (output_ncc == -1)
                output_ncc = 1;
            reg_ncc[inputIndex] = output_ncc;
            mean_ncc += output_ncc;

            // set slice weight
            if (output_ncc > _global_NCC_threshold)
                _reg_slice_weight[inputIndex] = 1;
            else {
                _reg_slice_weight[inputIndex] = -1;
                cout << inputIndex << " ";
            }
        }
        cout << endl;
        mean_ncc /= _slices.size();

        cout << " - mean registration ncc: " << mean_ncc << endl;

        SVRTK_END_TIMING("StructuralExclusion");
    }

    //-------------------------------------------------------------------

    // run SVR
    void Reconstruction::SliceToVolumeRegistration() {
        SVRTK_START_TIMING();

        if (_debug)
            _reconstructed.Write("target.nii.gz");

        _grey_reconstructed = _reconstructed;

        if (!_ffd) {
            Parallel::SliceToVolumeRegistration p_reg(this);
            p_reg();
        } else {
            Parallel::SliceToVolumeRegistrationFFD p_reg(this);
            p_reg();
        }

        SVRTK_END_TIMING("SliceToVolumeRegistration");
    }

    //-------------------------------------------------------------------

    // run remote SVR
    void Reconstruction::RemoteSliceToVolumeRegistration(int iter, const string& str_mirtk_path, const string& str_current_exchange_file_path) {
        SVRTK_START_TIMING();

        const ImageAttributes& attr_recon = _reconstructed.Attributes();
        const string str_source = str_current_exchange_file_path + "/current-source.nii.gz";
        _reconstructed.Write(str_source.c_str());

        RealImage target;
        ResamplingWithPadding<RealPixel> resampling(attr_recon._dx, attr_recon._dx, attr_recon._dx, -1);
        GenericLinearInterpolateImageFunction<RealImage> interpolator;
        resampling.Interpolator(&interpolator);
        resampling.Output(&target);

        constexpr int stride = 32;
        int svr_range_start = 0;
        int svr_range_stop = svr_range_start + stride;

        if (!_ffd) {
            // rigid SVR
            if (iter < 3) {
                _offset_matrices.clear();

                // save slice .nii.gz files
                // Do not parallelise: ResamplingWithPadding has already been parallelised!
                for (size_t inputIndex = 0; inputIndex < _slices.size(); inputIndex++) {
                    target.Initialize(_slices[inputIndex].Attributes());
                    resampling.Input(&_slices[inputIndex]);
                    resampling.Run();

                    // put origin to zero
                    RigidTransformation offset;
                    ResetOrigin(target, offset);

                    RealPixel tmin, tmax;
                    target.GetMinMax(&tmin, &tmax);
                    _zero_slices[inputIndex] = tmax > 1 && (tmax - tmin) > 1 ? 1 : -1;

                    const string str_target = str_current_exchange_file_path + "/res-slice-" + to_string(inputIndex) + ".nii.gz";
                    target.Write(str_target.c_str());

                    _offset_matrices.push_back(offset.GetMatrix());
                }
            }

            // save slice transformations
            #pragma omp parallel for
            for (size_t inputIndex = 0; inputIndex < _slices.size(); inputIndex++) {
                RigidTransformation r_transform = _transformations[inputIndex];
                r_transform.PutMatrix(r_transform.GetMatrix() * _offset_matrices[inputIndex]);

                const string str_dofin = str_current_exchange_file_path + "/res-transformation-" + to_string(inputIndex) + ".dof";
                r_transform.Write(str_dofin.c_str());
            }

            // run remote SVR in strides
            while (svr_range_start < _slices.size()) {
                Parallel::RemoteSliceToVolumeRegistration registration(this, svr_range_start, svr_range_stop, str_mirtk_path, str_current_exchange_file_path);
                registration();

                svr_range_start = svr_range_stop;
                svr_range_stop = min(svr_range_start + stride, (int)_slices.size());
            }

            // read output transformations
            #pragma omp parallel for
            for (size_t inputIndex = 0; inputIndex < _slices.size(); inputIndex++) {
                const string str_dofout = str_current_exchange_file_path + "/res-transformation-" + to_string(inputIndex) + ".dof";
                _transformations[inputIndex].Read(str_dofout.c_str());

                //undo the offset
                _transformations[inputIndex].PutMatrix(_transformations[inputIndex].GetMatrix() * _offset_matrices[inputIndex].Inverse());
            }
        } else {
            // FFD SVR
            if (iter < 3) {
                // save slice .nii.gz files and transformations
                // Do not parallelise: ResamplingWithPadding has already been parallelised!
                for (size_t inputIndex = 0; inputIndex < _slices.size(); inputIndex++) {
                    target.Initialize(_slices[inputIndex].Attributes());
                    resampling.Input(&_slices[inputIndex]);
                    resampling.Run();

                    RealPixel tmin, tmax;
                    target.GetMinMax(&tmin, &tmax);
                    _zero_slices[inputIndex] = tmax > 1 && (tmax - tmin) > 1 ? 1 : -1;

                    string str_target = str_current_exchange_file_path + "/slice-" + to_string(inputIndex) + ".nii.gz";
                    target.Write(str_target.c_str());

                    string str_dofin = str_current_exchange_file_path + "/transformation-" + to_string(inputIndex) + ".dof";
                    _mffd_transformations[inputIndex].Write(str_dofin.c_str());
                }
            }

            // run parallel remote FFD SVR in strides
            while (svr_range_start < _slices.size()) {
                Parallel::RemoteSliceToVolumeRegistration registration(this, svr_range_start, svr_range_stop, str_mirtk_path, str_current_exchange_file_path, false);
                registration();

                svr_range_start = svr_range_stop;
                svr_range_stop = min(svr_range_start + stride, (int)_slices.size());
            }

            // read output transformations
            #pragma omp parallel for
            for (size_t inputIndex = 0; inputIndex < _slices.size(); inputIndex++) {
                const string str_dofout = str_current_exchange_file_path + "/transformation-" + to_string(inputIndex) + ".dof";
                _mffd_transformations[inputIndex].Read(str_dofout.c_str());
            }
        }

        SVRTK_END_TIMING("RemoteSliceToVolumeRegistration");
    }

    //-------------------------------------------------------------------

    // save the current recon model (for remote reconstruction option) - can be deleted
    void Reconstruction::SaveModelRemote(const string& str_current_exchange_file_path, int status_flag, int current_iteration) {
        if (_verbose)
            _verbose_log << "SaveModelRemote : " << current_iteration << endl;

        // save slices
        if (status_flag > 0) {
            #pragma omp parallel for
            for (size_t inputIndex = 0; inputIndex < _slices.size(); inputIndex++) {
                const string str_slice = str_current_exchange_file_path + "/org-slice-" + to_string(inputIndex) + ".nii.gz";
                _slices[inputIndex].Write(str_slice.c_str());
            }
            const string str_mask = str_current_exchange_file_path + "/current-mask.nii.gz";
            _mask.Write(str_mask.c_str());
        }

        // save transformations
        #pragma omp parallel for
        for (size_t inputIndex = 0; inputIndex < _slices.size(); inputIndex++) {
            const string str_dofin = str_current_exchange_file_path + "/org-transformation-" + to_string(current_iteration) + "-" + to_string(inputIndex) + ".dof";
            _transformations[inputIndex].Write(str_dofin.c_str());
        }

        // save recon volume
        const string str_recon = str_current_exchange_file_path + "/latest-out-recon.nii.gz";
        _reconstructed.Write(str_recon.c_str());
    }

    //-------------------------------------------------------------------

    // load remotely reconstructed volume (for remote reconstruction option) - can be deleted
    void Reconstruction::LoadResultsRemote(const string& str_current_exchange_file_path, int current_number_of_slices, int current_iteration) {
        if (_verbose)
            _verbose_log << "LoadResultsRemote : " << current_iteration << endl;

        const string str_recon = str_current_exchange_file_path + "/latest-out-recon.nii.gz";
        _reconstructed.Read(str_recon.c_str());
    }

    // load the current recon model (for remote reconstruction option) - can be deleted
    void Reconstruction::LoadModelRemote(const string& str_current_exchange_file_path, int current_number_of_slices, double average_thickness, int current_iteration) {
        if (_verbose)
            _verbose_log << "LoadModelRemote : " << current_iteration << endl;

        const string str_recon = str_current_exchange_file_path + "/latest-out-recon.nii.gz";
        const string str_mask = str_current_exchange_file_path + "/current-mask.nii.gz";

        _reconstructed.Read(str_recon.c_str());
        _mask.Read(str_mask.c_str());

        _template_created = true;
        _grey_reconstructed = _reconstructed;
        _attr_reconstructed = _reconstructed.Attributes();

        _have_mask = true;

        for (int inputIndex = 0; inputIndex < current_number_of_slices; inputIndex++) {
            // load slices
            RealImage slice;
            const string str_slice = str_current_exchange_file_path + "/org-slice-" + to_string(inputIndex) + ".nii.gz";
            slice.Read(str_slice.c_str());
            slice.PutPixelSize(slice.GetXSize(), slice.GetYSize(), average_thickness);
            _slices.push_back(slice);

            // load transformations
            const string str_dofin = str_current_exchange_file_path + "/org-transformation-" + to_string(current_iteration) + "-" + to_string(inputIndex) + ".dof";
            Transformation *t = Transformation::New(str_dofin.c_str());
            unique_ptr<RigidTransformation> rigidTransf(dynamic_cast<RigidTransformation*>(t));
            _transformations.push_back(*rigidTransf);

            RealPixel tmin, tmax;
            slice.GetMinMax(&tmin, &tmax);
            _zero_slices.push_back(tmax > 1 && (tmax - tmin) > 1 ? 1 : -1);

            _package_index.push_back(0);

            _slice_attributes.push_back(slice.Attributes());

            _grey_slices.push_back(GreyImage(slice));

            memset(slice.Data(), 0, sizeof(RealPixel) * slice.NumberOfVoxels());
            _slice_dif.push_back(slice);
            _simulated_slices.push_back(slice);

            _reg_slice_weight.push_back(1);
            _slice_pos.push_back(inputIndex);

            slice = 1;
            _simulated_weights.push_back(slice);
            _simulated_inside.push_back(move(slice));

            _stack_index.push_back(0);

            if (_ffd)
                _mffd_transformations.push_back(MultiLevelFreeFormTransformation());
        }
    }

    //-------------------------------------------------------------------

    // save slice info in .csv
    void Reconstruction::SaveSliceInfo(int current_iteration) {
        string file_name;
        ofstream GD_csv_file;

        if (current_iteration > 0)
            file_name = (boost::format("summary-slice-info-%1%.csv") % current_iteration).str();
        else
            file_name = "summary-slice-info.csv";

        GD_csv_file.open(file_name);
        GD_csv_file << "Stack" << "," << "Slice" << "," << "Rx" << "," << "Ry" << "," << "Rz" << "," << "Tx" << "," << "Ty" << "," << "Tz" << "," << "Weight" << "," << "Inside" << "," << "Scale" << endl;

        for (size_t i = 0; i < _slices.size(); i++) {
            const double rx = _transformations[i].GetRotationX();
            const double ry = _transformations[i].GetRotationY();
            const double rz = _transformations[i].GetRotationZ();

            const double tx = _transformations[i].GetTranslationX();
            const double ty = _transformations[i].GetTranslationY();
            const double tz = _transformations[i].GetTranslationZ();

            const int inside = _slice_inside[i] ? 1 : 0;

            GD_csv_file << _stack_index[i] << "," << i << "," << rx << "," << ry << "," << rz << "," << tx << "," << ty << "," << tz << "," << _slice_weight[i] << "," << inside << "," << _scale[i] << endl;
        }

        GD_csv_file.close();
    }

    //-------------------------------------------------------------------

    // perform nonlocal means filtering
    void Reconstruction::NLMFiltering(Array<RealImage>& stacks) {
        NLDenoising denoising;
        for (int i = 0; i < stacks.size(); i++) {
            stacks[i] = denoising.Run(stacks[i], 3, 1);
            stacks[i].Write((boost::format("denoised-%1%.nii.gz") % i).str().c_str());
        }
    }

    //-------------------------------------------------------------------

    // run calculation of transformation matrices
    void Reconstruction::CoeffInit() {
        SVRTK_START_TIMING();

        //resize slice-volume matrix from previous iteration
        ClearAndResize(_volcoeffs, _slices.size());

        //resize indicator of slice having and overlap with volumetric mask
        ClearAndResize(_slice_inside, _slices.size());
        _attr_reconstructed = _reconstructed.Attributes();

        Parallel::CoeffInit coeffinit(this);
        coeffinit();

        //prepare image for volume weights, will be needed for Gaussian Reconstruction
        _volume_weights.Initialize(_reconstructed.Attributes());

        // Do not parallelise: It would cause data inconsistencies
        for (size_t inputIndex = 0; inputIndex < _slices.size(); inputIndex++) {
            bool excluded = false;

            for (size_t fe = 0; fe < _force_excluded.size(); fe++) {
                if (inputIndex == _force_excluded[fe]) {
                    excluded = true;
                    break;
                }
            }

            if (!excluded) {
                // Do not parallelise: It would cause data inconsistencies
                for (int i = 0; i < _slices[inputIndex].GetX(); i++)
                    for (int j = 0; j < _slices[inputIndex].GetY(); j++)
                        for (size_t k = 0; k < _volcoeffs[inputIndex][i][j].size(); k++) {
                            const POINT3D& p = _volcoeffs[inputIndex][i][j][k];
                            _volume_weights(p.x, p.y, p.z) += p.value;
                        }
            }
        }

        if (_debug)
            _volume_weights.Write("volume_weights.nii.gz");

        //find average volume weight to modify alpha parameters accordingly
        const RealPixel *ptr = _volume_weights.Data();
        const RealPixel *pm = _mask.Data();
        double sum = 0;
        int num = 0;
        #pragma omp parallel for reduction(+: sum, num)
        for (int i = 0; i < _volume_weights.NumberOfVoxels(); i++) {
            if (pm[i] == 1) {
                sum += ptr[i];
                num++;
            }
        }
        _average_volume_weight = sum / num;

        if (_verbose)
            _verbose_log << "Average volume weight is " << _average_volume_weight << endl;

        SVRTK_END_TIMING("CoeffInit");
    }

    //-------------------------------------------------------------------

    // run gaussian reconstruction based on SVR & coeffinit outputs
    void Reconstruction::GaussianReconstruction() {
        SVRTK_START_TIMING();

        RealImage slice;
        Array<int> voxel_num;
        voxel_num.reserve(_slices.size());

        //clear _reconstructed image
        memset(_reconstructed.Data(), 0, sizeof(RealPixel) * _reconstructed.NumberOfVoxels());

        for (size_t inputIndex = 0; inputIndex < _slices.size(); inputIndex++) {
            bool excluded = false;

            for (size_t fe = 0; fe < _force_excluded.size(); fe++) {
                if (inputIndex == _force_excluded[fe]) {
                    excluded = true;
                    break;
                }
            }

            if (excluded)
                continue;

            int slice_vox_num = 0;
            //copy the current slice
            slice = _slices[inputIndex];
            //alias the current bias image
            const RealImage& b = _bias[inputIndex];
            //read current scale factor
            const double scale = _scale[inputIndex];

            //Distribute slice intensities to the volume
            for (int i = 0; i < slice.GetX(); i++)
                for (int j = 0; j < slice.GetY(); j++)
                    if (slice(i, j, 0) > -0.01) {
                        //biascorrect and scale the slice
                        slice(i, j, 0) *= exp(-b(i, j, 0)) * scale;

                        //number of volume voxels with non-zero coefficients
                        //for current slice voxel
                        const size_t n = _volcoeffs[inputIndex][i][j].size();

                        //if given voxel is not present in reconstructed volume at all, pad it

                        //calculate num of vox in a slice that have overlap with roi
                        if (n > 0)
                            slice_vox_num++;

                        //add contribution of current slice voxel to all voxel volumes
                        //to which it contributes
                        for (size_t k = 0; k < n; k++) {
                            const POINT3D& p = _volcoeffs[inputIndex][i][j][k];
                            _reconstructed(p.x, p.y, p.z) += p.value * slice(i, j, 0);
                        }
                    }
            voxel_num.push_back(slice_vox_num);
            //end of loop for a slice inputIndex
        }

        //normalize the volume by proportion of contributing slice voxels
        //for each volume voxel
        _reconstructed /= _volume_weights;

        _reconstructed.Write("init.nii.gz");

        // find slices with small overlap with ROI and exclude them.
        //find median
        Array<int> voxel_num_tmp = voxel_num;
        int median = round(voxel_num_tmp.size() * 0.5) - 1;
        nth_element(voxel_num_tmp.begin(), voxel_num_tmp.begin() + median, voxel_num_tmp.end());
        median = voxel_num_tmp[median];

        //remember slices with small overlap with ROI
        ClearAndReserve(_small_slices, voxel_num.size());
        for (size_t i = 0; i < voxel_num.size(); i++)
            if (voxel_num[i] < 0.1 * median)
                _small_slices.push_back(i);

        if (_verbose) {
            _verbose_log << "Small slices:";
            for (size_t i = 0; i < _small_slices.size(); i++)
                _verbose_log << " " << _small_slices[i];
            _verbose_log << endl;
        }

        SVRTK_END_TIMING("GaussianReconstruction");
    }

    //-------------------------------------------------------------------

    // another version of CoeffInit
    void Reconstruction::CoeffInitSF(int begin, int end) {
        //resize slice-volume matrix from previous iteration
        ClearAndResize(_volcoeffsSF, _slicePerDyn);

        //resize indicator of slice having and overlap with volumetric mask
        ClearAndResize(_slice_insideSF, _slicePerDyn);

        Parallel::CoeffInitSF coeffinit(this, begin, end);
        coeffinit();

        //prepare image for volume weights, will be needed for Gaussian Reconstruction
        _volume_weightsSF.Initialize(_reconstructed.Attributes());

        const Array<RealImage>& slices = _withMB ? _slicesRwithMB : _slices;

        // Do not parallelise: It would cause data inconsistencies
        for (int inputIndex = begin; inputIndex < end; inputIndex++)
            for (int i = 0; i < slices[inputIndex].GetX(); i++)
                for (int j = 0; j < slices[inputIndex].GetY(); j++)
                    for (size_t k = 0; k < _volcoeffsSF[inputIndex % _slicePerDyn][i][j].size(); k++) {
                        const POINT3D& p = _volcoeffsSF[inputIndex % _slicePerDyn][i][j][k];
                        _volume_weightsSF(p.x, p.y, p.z) += p.value;
                    }

        if (_debug)
            _volume_weightsSF.Write("volume_weights.nii.gz");

        //find average volume weight to modify alpha parameters accordingly
        const RealPixel *ptr = _volume_weightsSF.Data();
        const RealPixel *pm = _mask.Data();
        double sum = 0;
        int num = 0;
        #pragma omp parallel for reduction(+: sum, num)
        for (int i = 0; i < _volume_weightsSF.NumberOfVoxels(); i++) {
            if (pm[i] == 1) {
                sum += ptr[i];
                num++;
            }
        }
        _average_volume_weightSF = sum / num;

        if (_verbose)
            _verbose_log << "Average volume weight is " << _average_volume_weightSF << endl;

    }  //end of CoeffInitSF()

    //-------------------------------------------------------------------

    // another version of gaussian reconstruction
    void Reconstruction::GaussianReconstructionSF(const Array<RealImage>& stacks) {
        Array<int> voxel_num;
        Array<RigidTransformation> currentTransformations;
        Array<RealImage> currentSlices, currentBiases;
        Array<double> currentScales;
        RealImage interpolated, slice;

        // Preallocate memory
        const size_t reserve_size = stacks[0].Attributes()._z;
        voxel_num.reserve(reserve_size);
        currentTransformations.reserve(reserve_size);
        currentSlices.reserve(reserve_size);
        currentScales.reserve(reserve_size);
        currentBiases.reserve(reserve_size);

        // clean _reconstructed
        memset(_reconstructed.Data(), 0, sizeof(RealPixel) * _reconstructed.NumberOfVoxels());

        for (size_t dyn = 0, counter = 0; dyn < stacks.size(); dyn++) {
            const ImageAttributes& attr = stacks[dyn].Attributes();

            CoeffInitSF(counter, counter + attr._z);

            for (int s = 0; s < attr._z; s++) {
                currentTransformations.push_back(_transformations[counter + s]);
                currentSlices.push_back(_slices[counter + s]);
                currentScales.push_back(_scale[counter + s]);
                currentBiases.push_back(_bias[counter + s]);
            }

            interpolated.Initialize(_reconstructed.Attributes());

            for (size_t s = 0; s < currentSlices.size(); s++) {
                //copy the current slice
                slice = currentSlices[s];
                //alias the current bias image
                const RealImage& b = currentBiases[s];
                //read current scale factor
                const double scale = currentScales[s];

                int slice_vox_num = 0;
                for (int i = 0; i < slice.GetX(); i++)
                    for (int j = 0; j < slice.GetY(); j++)
                        if (slice(i, j, 0) > -0.01) {
                            //biascorrect and scale the slice
                            slice(i, j, 0) *= exp(-b(i, j, 0)) * scale;

                            //number of volume voxels with non-zero coefficients for current slice voxel
                            const size_t n = _volcoeffsSF[s][i][j].size();

                            //if given voxel is not present in reconstructed volume at all, pad it

                            //calculate num of vox in a slice that have overlap with roi
                            if (n > 0)
                                slice_vox_num++;

                            //add contribution of current slice voxel to all voxel volumes
                            //to which it contributes
                            for (size_t k = 0; k < n; k++) {
                                const POINT3D& p = _volcoeffsSF[s][i][j][k];
                                interpolated(p.x, p.y, p.z) += p.value * slice(i, j, 0);
                            }
                        }
                voxel_num.push_back(slice_vox_num);
            }
            counter += attr._z;
            _reconstructed += interpolated / _volume_weightsSF;

            currentTransformations.clear();
            currentSlices.clear();
            currentScales.clear();
            currentBiases.clear();
        }
        _reconstructed /= stacks.size();

        cout << "done." << endl;
        if (_debug)
            _reconstructed.Write("init.nii.gz");

        //now find slices with small overlap with ROI and exclude them.
        //find median
        Array<int> voxel_num_tmp = voxel_num;
        int median = round(voxel_num_tmp.size() * 0.5) - 1;
        nth_element(voxel_num_tmp.begin(), voxel_num_tmp.begin() + median, voxel_num_tmp.end());
        median = voxel_num_tmp[median];

        //remember slices with small overlap with ROI
        ClearAndReserve(_small_slices, voxel_num.size());
        for (size_t i = 0; i < voxel_num.size(); i++)
            if (voxel_num[i] < 0.1 * median)
                _small_slices.push_back(i);

        if (_verbose) {
            _verbose_log << "Small slices:";
            for (size_t i = 0; i < _small_slices.size(); i++)
                _verbose_log << " " << _small_slices[i];
            _verbose_log << endl;
        }
    }

    //-------------------------------------------------------------------

    // initialise slice EM step
    void Reconstruction::InitializeEM() {
        ClearAndReserve(_weights, _slices.size());
        ClearAndReserve(_bias, _slices.size());
        ClearAndReserve(_scale, _slices.size());
        ClearAndReserve(_slice_weight, _slices.size());

        for (size_t i = 0; i < _slices.size(); i++) {
            //Create images for voxel weights and bias fields
            _weights.push_back(_slices[i]);
            _bias.push_back(_slices[i]);

            //Create and initialize scales
            _scale.push_back(1);

            //Create and initialize slice weights
            _slice_weight.push_back(1);
        }

        //Find the range of intensities
        _max_intensity = voxel_limits<RealPixel>::min();
        _min_intensity = voxel_limits<RealPixel>::max();

        #pragma omp parallel for reduction(min: _min_intensity) reduction(max: _max_intensity)
        for (size_t i = 0; i < _slices.size(); i++) {
            //to update minimum we need to exclude padding value
            const RealPixel *ptr = _slices[i].Data();
            for (int ind = 0; ind < _slices[i].NumberOfVoxels(); ind++) {
                if (ptr[ind] > 0) {
                    if (ptr[ind] > _max_intensity)
                        _max_intensity = ptr[ind];
                    if (ptr[ind] < _min_intensity)
                        _min_intensity = ptr[ind];
                }
            }
        }
    }

    //-------------------------------------------------------------------

    // initialise / reset EM values
    void Reconstruction::InitializeEMValues() {
        SVRTK_START_TIMING();

        #pragma omp parallel for
        for (size_t i = 0; i < _slices.size(); i++) {
            //Initialise voxel weights and bias values
            const RealPixel *pi = _slices[i].Data();
            RealPixel *pw = _weights[i].Data();
            RealPixel *pb = _bias[i].Data();
            for (int j = 0; j < _weights[i].NumberOfVoxels(); j++) {
                pw[j] = pi[j] > -0.01 ? 1 : 0;
                pb[j] = 0;
            }

            //Initialise slice weights
            _slice_weight[i] = 1;

            //Initialise scaling factors for intensity matching
            _scale[i] = 1;
        }

        //Force exclusion of slices predefined by user
        for (size_t i = 0; i < _force_excluded.size(); i++)
            if (_force_excluded[i] > 0 && _force_excluded[i] < _slices.size())
                _slice_weight[_force_excluded[i]] = 0;

        SVRTK_END_TIMING("InitializeEMValues");
    }

    //-------------------------------------------------------------------

    // initialise parameters of EM robust statistics
    void Reconstruction::InitializeRobustStatistics() {
        Array<int> sigma_numbers(_slices.size());
        Array<double> sigma_values(_slices.size());
        RealImage slice;

        #pragma omp parallel for private(slice)
        for (size_t inputIndex = 0; inputIndex < _slices.size(); inputIndex++) {
            slice = _slices[inputIndex];
            //Voxel-wise sigma will be set to stdev of volumetric errors
            //For each slice voxel
            for (int i = 0; i < slice.GetX(); i++)
                for (int j = 0; j < slice.GetY(); j++)
                    if (slice(i, j, 0) > -0.01) {
                        //calculate stev of the errors
                        if (_simulated_inside[inputIndex](i, j, 0) == 1 && _simulated_weights[inputIndex](i, j, 0) > 0.99) {
                            slice(i, j, 0) -= _simulated_slices[inputIndex](i, j, 0);
                            sigma_values[inputIndex] += slice(i, j, 0) * slice(i, j, 0);
                            sigma_numbers[inputIndex]++;
                        }
                    }

            //if slice does not have an overlap with ROI, set its weight to zero
            if (!_slice_inside[inputIndex])
                _slice_weight[inputIndex] = 0;
        }

        double sigma = 0;
        int num = 0;
        #pragma omp simd
        for (size_t inputIndex = 0; inputIndex < _slices.size(); inputIndex++) {
            sigma += sigma_values[inputIndex];
            num += sigma_numbers[inputIndex];
        }

        //Force exclusion of slices predefined by user
        for (size_t i = 0; i < _force_excluded.size(); i++)
            if (_force_excluded[i] > 0 && _force_excluded[i] < _slices.size())
                _slice_weight[_force_excluded[i]] = 0;

        //initialize sigma for voxel-wise robust statistics
        _sigma = sigma / num;

        //initialize sigma for slice-wise robust statistics
        _sigma_s = 0.025;
        //initialize mixing proportion for inlier class in voxel-wise robust statistics
        _mix = 0.9;
        //initialize mixing proportion for outlier class in slice-wise robust statistics
        _mix_s = 0.9;
        //Initialise value for uniform distribution according to the range of intensities
        _m = 1 / (2.1 * _max_intensity - 1.9 * _min_intensity);

        if (_verbose)
            _verbose_log << "Initializing robust statistics: sigma=" << sqrt(_sigma) << " m=" << _m << " mix=" << _mix << " mix_s=" << _mix_s << endl;
    }

    //-------------------------------------------------------------------

    // run EStep for calculation of voxel-wise and slice-wise posteriors (weights)
    void Reconstruction::EStep() {
        Array<double> slice_potential(_slices.size());

        Parallel::EStep parallelEStep(this, slice_potential);
        parallelEStep();

        //To force-exclude slices predefined by a user, set their potentials to -1
        for (size_t i = 0; i < _force_excluded.size(); i++)
            if (_force_excluded[i] > 0 && _force_excluded[i] < _slices.size())
                slice_potential[_force_excluded[i]] = -1;

        //exclude slices identified as having small overlap with ROI, set their potentials to -1
        for (size_t i = 0; i < _small_slices.size(); i++)
            slice_potential[_small_slices[i]] = -1;

        //these are unrealistic scales pointing at misregistration - exclude the corresponding slices
        for (size_t inputIndex = 0; inputIndex < slice_potential.size(); inputIndex++)
            if (_scale[inputIndex] < 0.2 || _scale[inputIndex] > 5)
                slice_potential[inputIndex] = -1;

        //Calculation of slice-wise robust statistics parameters.
        //This is theoretically M-step,
        //but we want to use latest estimate of slice potentials
        //to update the parameters

        if (_verbose) {
            _verbose_log << endl << "Slice potentials:";
            for (size_t inputIndex = 0; inputIndex < slice_potential.size(); inputIndex++)
                _verbose_log << " " << slice_potential[inputIndex];
            _verbose_log << endl;
        }

        //Calculate means of the inlier and outlier potentials
        double sum = 0, den = 0, sum2 = 0, den2 = 0, maxs = 0, mins = 1;
        #pragma omp parallel for reduction(+: sum, sum2, den, den2) reduction(max: maxs) reduction(min: mins)
        for (size_t inputIndex = 0; inputIndex < _slices.size(); inputIndex++)
            if (slice_potential[inputIndex] >= 0) {
                //calculate means
                sum += slice_potential[inputIndex] * _slice_weight[inputIndex];
                den += _slice_weight[inputIndex];
                sum2 += slice_potential[inputIndex] * (1 - _slice_weight[inputIndex]);
                den2 += 1 - _slice_weight[inputIndex];

                //calculate min and max of potentials in case means need to be initalized
                if (slice_potential[inputIndex] > maxs)
                    maxs = slice_potential[inputIndex];
                if (slice_potential[inputIndex] < mins)
                    mins = slice_potential[inputIndex];
            }

        _mean_s = den > 0 ? sum / den : mins;
        _mean_s2 = den2 > 0 ? sum2 / den2 : (maxs + _mean_s) / 2;

        //Calculate the variances of the potentials
        sum = 0; den = 0; sum2 = 0; den2 = 0;
        #pragma omp parallel for reduction(+: sum, sum2, den, den2)
        for (size_t inputIndex = 0; inputIndex < _slices.size(); inputIndex++)
            if (slice_potential[inputIndex] >= 0) {
                sum += (slice_potential[inputIndex] - _mean_s) * (slice_potential[inputIndex] - _mean_s) * _slice_weight[inputIndex];
                den += _slice_weight[inputIndex];
                sum2 += (slice_potential[inputIndex] - _mean_s2) * (slice_potential[inputIndex] - _mean_s2) * (1 - _slice_weight[inputIndex]);
                den2 += 1 - _slice_weight[inputIndex];
            }

        //_sigma_s
        if (sum > 0 && den > 0) {
            //do not allow too small sigma
            _sigma_s = max(sum / den, _step * _step / 6.28);
        } else {
            _sigma_s = 0.025;
            if (_verbose) {
                if (sum <= 0)
                    _verbose_log << "All slices are equal. ";
                if (den < 0) //this should not happen
                    _verbose_log << "All slices are outliers. ";
                _verbose_log << "Setting sigma to " << sqrt(_sigma_s) << endl;
            }
        }

        //sigma_s2
        if (sum2 > 0 && den2 > 0) {
            //do not allow too small sigma
            _sigma_s2 = max(sum2 / den2, _step * _step / 6.28);
        } else {
            //do not allow too small sigma
            _sigma_s2 = max((_mean_s2 - _mean_s) * (_mean_s2 - _mean_s) / 4, _step * _step / 6.28);

            if (_verbose) {
                if (sum2 <= 0)
                    _verbose_log << "All slices are equal. ";
                if (den2 <= 0)
                    _verbose_log << "All slices inliers. ";
                _verbose_log << "Setting sigma_s2 to " << sqrt(_sigma_s2) << endl;
            }
        }

        //Calculate slice weights
        #pragma omp parallel for
        for (size_t inputIndex = 0; inputIndex < _slices.size(); inputIndex++) {
            //Slice does not have any voxels in volumetric ROI
            if (slice_potential[inputIndex] == -1) {
                _slice_weight[inputIndex] = 0;
                continue;
            }

            //All slices are outliers or the means are not valid
            if (den <= 0 || _mean_s2 <= _mean_s) {
                _slice_weight[inputIndex] = 1;
                continue;
            }

            //likelihood for inliers
            const double gs1 = slice_potential[inputIndex] < _mean_s2 ? G(slice_potential[inputIndex] - _mean_s, _sigma_s) : 0;

            //likelihood for outliers
            const double gs2 = slice_potential[inputIndex] > _mean_s ? G(slice_potential[inputIndex] - _mean_s2, _sigma_s2) : 0;

            //calculate slice weight
            const double likelihood = gs1 * _mix_s + gs2 * (1 - _mix_s);
            if (likelihood > 0)
                _slice_weight[inputIndex] = gs1 * _mix_s / likelihood;
            else {
                if (slice_potential[inputIndex] <= _mean_s)
                    _slice_weight[inputIndex] = 1;
                if (slice_potential[inputIndex] >= _mean_s2)
                    _slice_weight[inputIndex] = 0;
                if (slice_potential[inputIndex] < _mean_s2 && slice_potential[inputIndex] > _mean_s) //should not happen
                    _slice_weight[inputIndex] = 1;
            }
        }

        //Update _mix_s this should also be part of MStep
        int num = 0; sum = 0;
        for (size_t inputIndex = 0; inputIndex < _slices.size(); inputIndex++)
            if (slice_potential[inputIndex] >= 0) {
                sum += _slice_weight[inputIndex];
                num++;
            }

        if (num > 0)
            _mix_s = sum / num;
        else {
            cout << "All slices are outliers. Setting _mix_s to 0.9." << endl;
            _mix_s = 0.9;
        }

        if (_verbose) {
            _verbose_log << setprecision(3);
            _verbose_log << "Slice robust statistics parameters: ";
            _verbose_log << "means: " << _mean_s << " " << _mean_s2 << "  ";
            _verbose_log << "sigmas: " << sqrt(_sigma_s) << " " << sqrt(_sigma_s2) << "  ";
            _verbose_log << "proportions: " << _mix_s << " " << 1 - _mix_s << endl;
            _verbose_log << "Slice weights:";
            for (size_t inputIndex = 0; inputIndex < _slices.size(); inputIndex++)
                _verbose_log << " " << _slice_weight[inputIndex];
            _verbose_log << endl;
        }
    }

    //-------------------------------------------------------------------

    // run slice scaling
    void Reconstruction::Scale() {
        SVRTK_START_TIMING();

        Parallel::Scale parallelScale(this);
        parallelScale();

        if (_verbose) {
            _verbose_log << setprecision(3);
            _verbose_log << "Slice scale =";
            for (size_t inputIndex = 0; inputIndex < _slices.size(); inputIndex++)
                _verbose_log << " " << _scale[inputIndex];
            _verbose_log << endl;
        }

        SVRTK_END_TIMING("Scale");
    }

    //-------------------------------------------------------------------

    // run slice bias correction
    void Reconstruction::Bias() {
        SVRTK_START_TIMING();
        Parallel::Bias parallelBias(this);
        parallelBias();
        SVRTK_END_TIMING("Bias");
    }

    //-------------------------------------------------------------------

    // compute difference between simulated and original slices
    void Reconstruction::SliceDifference() {
        #pragma omp parallel for
        for (size_t inputIndex = 0; inputIndex < _slices.size(); inputIndex++) {
            _slice_dif[inputIndex] = _slices[inputIndex];

            for (int i = 0; i < _slices[inputIndex].GetX(); i++) {
                for (int j = 0; j < _slices[inputIndex].GetY(); j++) {
                    if (_slices[inputIndex](i, j, 0) > -0.01) {
                        _slice_dif[inputIndex](i, j, 0) *= exp(-(_bias[inputIndex])(i, j, 0)) * _scale[inputIndex];
                        _slice_dif[inputIndex](i, j, 0) -= _simulated_slices[inputIndex](i, j, 0);
                    } else
                        _slice_dif[inputIndex](i, j, 0) = 0;
                }
            }
        }
    }

    //-------------------------------------------------------------------

    // run SR reconstruction step
    void Reconstruction::Superresolution(int iter) {
        SVRTK_START_TIMING();

        // save current reconstruction for edge-preserving smoothing
        RealImage original = _reconstructed;

        SliceDifference();

        Parallel::Superresolution parallelSuperresolution(this);
        parallelSuperresolution();

        RealImage& addon = parallelSuperresolution.addon;
        _confidence_map = move(parallelSuperresolution.confidence_map);
        //_confidence4mask = _confidence_map;

        if (_debug) {
            _confidence_map.Write((boost::format("confidence-map%1%.nii.gz") % iter).str().c_str());
            addon.Write((boost::format("addon%1%.nii.gz") % iter).str().c_str());
        }

        if (!_adaptive) {
            RealPixel *pa = addon.Data();
            RealPixel *pcm = _confidence_map.Data();
            #pragma omp parallel for
            for (int i = 0; i < addon.NumberOfVoxels(); i++) {
                if (pcm[i] > 0) {
                    // ISSUES if pcm[i] is too small leading to bright pixels
                    pa[i] /= pcm[i];
                    //this is to revert to normal (non-adaptive) regularisation
                    pcm[i] = 1;
                }
            }
        }

        // update the volume with computed addon
        _reconstructed += addon * _alpha; //_average_volume_weight;

        //bound the intensities
        RealPixel *pr = _reconstructed.Data();
        #pragma omp parallel for
        for (int i = 0; i < _reconstructed.NumberOfVoxels(); i++) {
            if (pr[i] < _min_intensity * 0.9)
                pr[i] = _min_intensity * 0.9;
            if (pr[i] > _max_intensity * 1.1)
                pr[i] = _max_intensity * 1.1;
        }

        //Smooth the reconstructed image with regularisation
        AdaptiveRegularization(iter, original);

        //Remove the bias in the reconstructed volume compared to previous iteration
        if (_global_bias_correction)
            BiasCorrectVolume(original);

        SVRTK_END_TIMING("Superresolution");
    }

    //-------------------------------------------------------------------

    // run MStep (RS)
    void Reconstruction::MStep(int iter) {
        Parallel::MStep parallelMStep(this);
        parallelMStep();
        const double sigma = parallelMStep.sigma;
        const double mix = parallelMStep.mix;
        const double num = parallelMStep.num;
        const double min = parallelMStep.min;
        const double max = parallelMStep.max;

        //Calculate sigma and mix
        if (mix > 0) {
            _sigma = sigma / mix;
        } else {
            cerr << "Something went wrong: sigma=" << sigma << " mix=" << mix << endl;
            exit(1);
        }
        if (_sigma < _step * _step / 6.28)
            _sigma = _step * _step / 6.28;
        if (iter > 1)
            _mix = mix / num;

        //Calculate m
        _m = 1 / (max - min);

        if (_verbose)
            _verbose_log << "Voxel-wise robust statistics parameters: sigma=" << sqrt(_sigma) << " mix=" << _mix << " m=" << _m << endl;
    }

    //-------------------------------------------------------------------

    // run adaptive regularisation of the SR reconstructed volume
    void Reconstruction::AdaptiveRegularization(int iter, const RealImage& original) {
        Array<double> factor(13);
        for (int i = 0; i < 13; i++) {
            for (int j = 0; j < 3; j++)
                factor[i] += fabs(double(_directions[i][j]));
            factor[i] = 1 / factor[i];
        }

        Array<RealImage> b(13, _reconstructed);

        Parallel::AdaptiveRegularization1 parallelAdaptiveRegularization1(this, b, factor, original);
        parallelAdaptiveRegularization1();

        const RealImage original2 = _reconstructed;
        Parallel::AdaptiveRegularization2 parallelAdaptiveRegularization2(this, b, original2);
        parallelAdaptiveRegularization2();

        if (_alpha * _lambda / (_delta * _delta) > 0.068)
            cerr << "Warning: regularization might not have smoothing effect! Ensure that alpha*lambda/delta^2 is below 0.068." << endl;
    }

    //-------------------------------------------------------------------

    // correct bias
    void Reconstruction::BiasCorrectVolume(const RealImage& original) {
        //remove low-frequency component in the reconstructed image which might have occurred due to overfitting of the biasfield
        RealImage residual = _reconstructed;
        RealImage weights = _mask;

        //calculate weighted residual
        const RealPixel *po = original.Data();
        RealPixel *pr = residual.Data();
        RealPixel *pw = weights.Data();
        #pragma omp parallel for
        for (int i = 0; i < _reconstructed.NumberOfVoxels(); i++) {
            //second and term to avoid numerical problems
            if ((pw[i] == 1) && (po[i] > _low_intensity_cutoff * _max_intensity) && (pr[i] > _low_intensity_cutoff * _max_intensity)) {
                pr[i] = log(pr[i] / po[i]);
            } else {
                pw[i] = pr[i] = 0;
            }
        }

        //blurring needs to be same as for slices
        GaussianBlurring<RealPixel> gb(_sigma_bias);
        //blur weighted residual
        gb.Input(&residual);
        gb.Output(&residual);
        gb.Run();
        //blur weight image
        gb.Input(&weights);
        gb.Output(&weights);
        gb.Run();

        //calculate the bias field
        pr = residual.Data();
        pw = weights.Data();
        const RealPixel *pm = _mask.Data();
        RealPixel *pi = _reconstructed.Data();
        #pragma omp parallel for
        for (int i = 0; i < _reconstructed.NumberOfVoxels(); i++) {
            if (pm[i] == 1) {
                //weighted gaussian smoothing
                //exponential to recover multiplicative bias field
                pr[i] = exp(pr[i] / pw[i]);
                //bias correct reconstructed
                pi[i] /= pr[i];
                //clamp intensities to allowed range
                if (pi[i] < _min_intensity * 0.9)
                    pi[i] = _min_intensity * 0.9;
                if (pi[i] > _max_intensity * 1.1)
                    pi[i] = _max_intensity * 1.1;
            } else {
                pr[i] = 0;
            }
        }
    }

    //-------------------------------------------------------------------

    // evaluation based on the number of excluded slices
    void Reconstruction::Evaluate(int iter, ostream& outstr) {
        outstr << "Iteration " << iter << ": " << endl;

        size_t included_count = 0, excluded_count = 0, outside_count = 0;
        string included, excluded, outside;

        for (size_t i = 0; i < _slices.size(); i++) {
            if (_slice_inside[i]) {
                if (_slice_weight[i] >= 0.5) {
                    included += " " + to_string(i);
                    included_count++;
                } else {
                    excluded += " " + to_string(i);
                    excluded_count++;
                }
            } else {
                outside += " " + to_string(i);
                outside_count++;
            }
        }

        outstr << "Included slices:" << included << "\n";
        outstr << "Total: " << included_count << "\n";
        outstr << "Excluded slices:" << excluded << "\n";
        outstr << "Total: " << excluded_count << "\n";
        outstr << "Outside slices:" << outside << "\n";
        outstr << "Total: " << outside_count << endl;
    }

    //-------------------------------------------------------------------

    // normalise Bias
    void Reconstruction::NormaliseBias(int iter) {
        SVRTK_START_TIMING();

        Parallel::NormaliseBias parallelNormaliseBias(this);
        parallelNormaliseBias();
        RealImage& bias = parallelNormaliseBias.bias;

        // normalize the volume by proportion of contributing slice voxels for each volume voxel
        bias /= _volume_weights;

        MaskImage(bias, _mask, 0);
        RealImage m = _mask;
        GaussianBlurring<RealPixel> gb(_sigma_bias);

        gb.Input(&bias);
        gb.Output(&bias);
        gb.Run();

        gb.Input(&m);
        gb.Output(&m);
        gb.Run();
        bias /= m;

        if (_debug)
            bias.Write((boost::format("averagebias%1%.nii.gz") % iter).str().c_str());

        RealPixel *pi = _reconstructed.Data();
        const RealPixel *pb = bias.Data();
        #pragma omp parallel for
        for (int i = 0; i < _reconstructed.NumberOfVoxels(); i++)
            if (pi[i] != -1)
                pi[i] /= exp(-(pb[i]));

        SVRTK_END_TIMING("NormaliseBias");
    }

    //-------------------------------------------------------------------

    void Reconstruction::ReadTransformations(const char *folder, size_t file_count, Array<RigidTransformation>& transformations) {
        if (_slices.size() == 0) {
            cerr << "Please create slices before reading transformations!" << endl;
            exit(1);
        }

        ClearAndResize(transformations, file_count);
        for (size_t i = 0; i < file_count; i++) {
            const string path = (boost::format("%1%/transformation%2%.dof") % (folder ? folder : ".") % i).str();
            Transformation *transformation = Transformation::New(path.c_str());
            unique_ptr<RigidTransformation> rigidTransf(dynamic_cast<RigidTransformation*>(transformation));
            transformations[i] = *rigidTransf;
            cout << path << endl;
        }
    }

    //-------------------------------------------------------------------

    void Reconstruction::ReadTransformations(const char *folder) {
        cout << "Reading transformations:" << endl;
        ReadTransformations(folder, _slices.size(), _transformations);
    }

    //-------------------------------------------------------------------

    void Reconstruction::SaveBiasFields() {
        #pragma omp parallel for
        for (size_t inputIndex = 0; inputIndex < _slices.size(); inputIndex++)
            _bias[inputIndex].Write((boost::format("bias%1%.nii.gz") % inputIndex).str().c_str());
    }

    //-------------------------------------------------------------------

    void Reconstruction::SaveConfidenceMap() {
        _confidence_map.Write("confidence-map.nii.gz");
    }

    //-------------------------------------------------------------------

    void Reconstruction::SaveSlices() {
        #pragma omp parallel for
        for (size_t inputIndex = 0; inputIndex < _slices.size(); inputIndex++)
            _slices[inputIndex].Write((boost::format("slice%1%.nii.gz") % inputIndex).str().c_str());
    }

    //-------------------------------------------------------------------

    void Reconstruction::SaveSlicesWithTiming() {
        cout << "Saving slices with timing: ";
        #pragma omp parallel for
        for (size_t inputIndex = 0; inputIndex < _slices.size(); inputIndex++)
            _slices[inputIndex].Write((boost::format("sliceTime%1%.nii.gz") % _slice_timing[inputIndex]).str().c_str());
    }

    //-------------------------------------------------------------------

    void Reconstruction::SaveSimulatedSlices() {
        cout << "Saving simulated slices ... ";
        #pragma omp parallel for
        for (size_t inputIndex = 0; inputIndex < _slices.size(); inputIndex++)
            _simulated_slices[inputIndex].Write((boost::format("simslice%1%.nii.gz") % inputIndex).str().c_str());
        cout << "done." << endl;
    }


    //-------------------------------------------------------------------

    void Reconstruction::SaveWeights() {
        #pragma omp parallel for
        for (size_t inputIndex = 0; inputIndex < _slices.size(); inputIndex++)
            _weights[inputIndex].Write((boost::format("weights%1%.nii.gz") % inputIndex).str().c_str());
    }

    //-------------------------------------------------------------------

    void Reconstruction::SaveRegistrationStep(const Array<RealImage>& stacks, const int step) {
        ImageAttributes attr = stacks[0].Attributes();
        int threshold = attr._z;
        int counter = 0;
        for (size_t inputIndex = 0; inputIndex < _slices.size(); inputIndex++) {
            if (inputIndex >= threshold) {
                counter++;
                attr = stacks[counter].Attributes();
                threshold += attr._z;
            }
            const int stack = counter;
            const int slice = inputIndex - (threshold - attr._z);
            _transformations[inputIndex].Write((boost::format("step%04i_travol%04islice%04i.dof") % step % stack % slice).str().c_str());
        }
    }

    //-------------------------------------------------------------------

    void Reconstruction::SaveTransformationsWithTiming(const int iter) {
        cout << "Saving transformations with timing: ";
        #pragma omp parallel for
        for (size_t i = 0; i < _transformations.size(); i++) {
            cout << i << " ";
            if (iter < 0)
                _transformations[i].Write((boost::format("transformationTime%1%.dof") % _slice_timing[i]).str().c_str());
            else
                _transformations[i].Write((boost::format("transformationTime%1%-%2%.dof") % iter % _slice_timing[i]).str().c_str());
        }
        cout << " done." << endl;
    }

    //-------------------------------------------------------------------

    void Reconstruction::SaveTransformations() {
        #pragma omp parallel for
        for (size_t i = 0; i < _transformations.size(); i++)
            _transformations[i].Write((boost::format("transformation%1%.dof") % i).str().c_str());
    }

    //-------------------------------------------------------------------

    void Reconstruction::SaveProbabilityMap(int i) {
        _brain_probability.Write((boost::format("probability_map%1%.nii") % i).str().c_str());
    }

    //-------------------------------------------------------------------

    // save slice info
    void Reconstruction::SlicesInfo(const char *filename, const Array<string>& stack_files) {
        ofstream info(filename);

        // header
        info << "stack_index\t"
            << "stack_name\t"
            << "included\t" // Included slices
            << "excluded\t"  // Excluded slices
            << "outside\t"  // Outside slices
            << "weight\t"
            << "scale\t"
            // << "_stack_factor\t"
            << "TranslationX\t"
            << "TranslationY\t"
            << "TranslationZ\t"
            << "RotationX\t"
            << "RotationY\t"
            << "RotationZ" << endl;

        for (size_t i = 0; i < _slices.size(); i++) {
            const RigidTransformation& t = _transformations[i];
            info << _stack_index[i] << "\t"
                << stack_files[_stack_index[i]] << "\t"
                << ((_slice_weight[i] >= 0.5 && _slice_inside[i]) ? 1 : 0) << "\t" // Included slices
                << ((_slice_weight[i] < 0.5 && _slice_inside[i]) ? 1 : 0) << "\t"  // Excluded slices
                << (!_slice_inside[i] ? 1 : 0) << "\t"  // Outside slices
                << _slice_weight[i] << "\t"
                << _scale[i] << "\t"
                // << _stack_factor[i] << "\t"
                << t.GetTranslationX() << "\t"
                << t.GetTranslationY() << "\t"
                << t.GetTranslationZ() << "\t"
                << t.GetRotationX() << "\t"
                << t.GetRotationY() << "\t"
                << t.GetRotationZ() << endl;
        }

        info.close();
    }

    //-------------------------------------------------------------------

    // get slice order parameters
    void Reconstruction::GetSliceAcquisitionOrder(const Array<RealImage>& stacks, const Array<int>& pack_num, const Array<int>& order, const int step, const int rewinder) {
        Array<int> realInterleaved, fakeAscending;

        for (size_t dyn = 0; dyn < stacks.size(); dyn++) {
            const ImageAttributes& attr = stacks[dyn].Attributes();
            const int slicesPerPackage = attr._z / pack_num[dyn];
            int z_slice_order[attr._z];
            int t_slice_order[attr._z];

            // Ascending or descending
            if (order[dyn] == 1 || order[dyn] == 2) {
                int counter = 0;
                int slice_pos_counter = 0;
                int p = 0; // package counter

                while (counter < attr._z) {
                    z_slice_order[counter] = slice_pos_counter;
                    t_slice_order[slice_pos_counter] = counter++;
                    slice_pos_counter += pack_num[dyn];

                    // start new package
                    if (order[dyn] == 1) {
                        if (slice_pos_counter >= attr._z)
                            slice_pos_counter = ++p;
                    } else {
                        if (slice_pos_counter < 0)
                            slice_pos_counter = attr._z - 1 - ++p;
                    }
                }
            } else {
                int rewinderFactor, stepFactor;

                if (order[dyn] == 3) {
                    rewinderFactor = 1;
                    stepFactor = 2;
                } else if (order[dyn] == 4) {
                    rewinderFactor = 1;
                } else {
                    stepFactor = step;
                    rewinderFactor = rewinder;
                }

                // pretending to do ascending within each package, and then shuffling according to interleaved acquisition
                for (int p = 0, counter = 0; p < pack_num[dyn]; p++) {
                    if (order[dyn] == 4) {
                        // getting step size, from PPE
                        if (attr._z - counter > slicesPerPackage * pack_num[dyn]) {
                            stepFactor = round(sqrt(double(slicesPerPackage + 1)));
                            counter++;
                        } else
                            stepFactor = round(sqrt(double(slicesPerPackage)));
                    }

                    // middle part of the stack
                    for (int s = 0; s < slicesPerPackage; s++) {
                        const int slice_pos_counter = s * pack_num[dyn] + p;
                        fakeAscending.push_back(slice_pos_counter);
                    }

                    // last slices for larger packages
                    if (attr._z > slicesPerPackage * pack_num[dyn]) {
                        const int slice_pos_counter = slicesPerPackage * pack_num[dyn] + p;
                        if (slice_pos_counter < attr._z)
                            fakeAscending.push_back(slice_pos_counter);
                    }

                    // shuffling
                    for (size_t i = 0, index = 0, restart = 0; i < fakeAscending.size(); i++, index += stepFactor) {
                        if (index >= fakeAscending.size()) {
                            restart += rewinderFactor;
                            index = restart;
                        }
                        realInterleaved.push_back(fakeAscending[index]);
                    }

                    fakeAscending.clear();
                }

                // saving
                for (int i = 0; i < attr._z; i++) {
                    z_slice_order[i] = realInterleaved[i];
                    t_slice_order[realInterleaved[i]] = i;
                }

                realInterleaved.clear();
            }

            // copying
            for (int i = 0; i < attr._z; i++) {
                _z_slice_order.push_back(z_slice_order[i]);
                _t_slice_order.push_back(t_slice_order[i]);
            }
        }
    }

    //-------------------------------------------------------------------

    // split images into varying N packages
    void Reconstruction::flexibleSplitImage(const Array<RealImage>& stacks, Array<RealImage>& sliceStacks, const Array<int>& pack_num, const Array<int>& sliceNums, const Array<int>& order, const int step, const int rewinder) {
        // calculate slice order
        GetSliceAcquisitionOrder(stacks, pack_num, order, step, rewinder);

        Array<int> z_internal_slice_order;
        z_internal_slice_order.reserve(stacks[0].Attributes()._z);

        // counters
        int counter1 = 0, counter2 = 0, counter3 = 0;
        int startIterations = 0;

        // dynamic loop
        for (size_t dyn = 0; dyn < stacks.size(); dyn++) {
            const RealImage& image = stacks[dyn];
            const ImageAttributes& attr = image.Attributes();
            // location acquisition order

            // slice loop
            for (int sl = 0; sl < attr._z; sl++)
                z_internal_slice_order.push_back(_z_slice_order[counter1 + sl]);

            // fake packages
            int sum = 0;
            while (sum < attr._z) {
                sum += sliceNums[counter2];
                counter2++;
            }

            // fake package loop
            const int endIterations = counter2;
            for (int iter = startIterations; iter < endIterations; iter++) {
                const int internalIterations = sliceNums[iter];
                RealImage stack(attr);

                // copying
                for (int sl = counter3; sl < internalIterations + counter3; sl++)
                    for (int j = 0; j < stack.GetY(); j++)
                        for (int i = 0; i < stack.GetX(); i++)
                            stack.Put(i, j, z_internal_slice_order[sl], image(i, j, z_internal_slice_order[sl]));

                // pushing package
                sliceStacks.push_back(move(stack));
                counter3 += internalIterations;

                // for (size_t i = 0; i < sliceStacks.size(); i++)
                //     sliceStacks[i].Write((boost::format("sliceStacks%1%.nii") % i).str().c_str());
            }

            // updating variables for next dynamic
            counter1 += attr._z;
            counter3 = 0;
            startIterations = endIterations;

            z_internal_slice_order.clear();
        }
    }

    //-------------------------------------------------------------------

    // split images based on multi-band acquisition
    void Reconstruction::flexibleSplitImagewithMB(const Array<RealImage>& stacks, Array<RealImage>& sliceStacks, const Array<int>& pack_num, const Array<int>& sliceNums, const Array<int>& multiband_vector, const Array<int>& order, const int step, const int rewinder) {
        // initializing variables
        Array<RealImage> chunks, chunksAll, chunks_separated, chunks_separated_reordered;
        Array<int> pack_num_chucks;
        int counter1, counter2, counter3, counter4, counter5;
        Array<int> sliceNumsChunks;

        int startFactor = 0, endFactor = 0;
        // dynamic loop
        for (size_t dyn = 0; dyn < stacks.size(); dyn++) {
            const RealImage& image = stacks[dyn];
            const ImageAttributes& attr = image.Attributes();
            const int multiband = multiband_vector[dyn];
            const int sliceMB = attr._z / multiband;
            int sum = 0;

            for (int m = 0; m < multiband; m++) {
                RealImage chunk = image.GetRegion(0, 0, m * sliceMB, attr._x, attr._y, (m + 1) * sliceMB);
                chunks.push_back(move(chunk));
                pack_num_chucks.push_back(pack_num[dyn]);
            }

            while (sum < sliceMB) {
                sum += sliceNums[endFactor];
                endFactor++;
            }

            for (int m = 0; m < multiband; m++)
                for (int iter = startFactor; iter < endFactor; iter++)
                    sliceNumsChunks.push_back(sliceNums[iter]);

            startFactor = endFactor;
        }

        // splitting each multiband subgroup
        flexibleSplitImage(chunks, chunksAll, pack_num_chucks, sliceNumsChunks, order, step, rewinder);

        counter4 = counter5 = 0;
        RealImage multibanded;
        // new dynamic loop
        for (size_t dyn = 0; dyn < stacks.size(); dyn++) {
            const RealImage& image = stacks[dyn];
            const ImageAttributes& attr = image.Attributes();
            const int multiband = multiband_vector[dyn];
            const int sliceMB = attr._z / multiband;
            int sum = 0, stepFactor = 0;
            multibanded.Initialize(attr);

            // stepping factor in vector
            while (sum < sliceMB) {
                sum += sliceNums[counter5 + stepFactor];
                stepFactor++;
            }

            // getting data from this dynamic
            for (int iter = 0; iter < multiband * stepFactor; iter++)
                chunks_separated.push_back(chunksAll[iter + counter4]);

            counter4 += multiband * stepFactor;

            // reordering chunks_separated
            counter1 = counter2 = counter3 = 0;
            while (counter1 < chunks_separated.size()) {
                chunks_separated_reordered.push_back(chunks_separated[counter2]);
                counter2 += stepFactor;
                if (counter2 > chunks_separated.size() - 1)
                    counter2 = ++counter3;
                counter1++;
            }

            // reassembling multiband packs
            counter1 = counter2 = 0;
            while (counter1 < chunks_separated_reordered.size()) {
                for (int m = 0; m < multiband; m++) {
                    const RealImage& toAdd = chunks_separated_reordered[counter1];
                    for (int k = 0; k < toAdd.GetZ(); k++) {
                        for (int j = 0; j < toAdd.GetY(); j++)
                            for (int i = 0; i < toAdd.GetX(); i++)
                                multibanded.Put(i, j, counter2, toAdd(i, j, k));

                        counter2++;
                    }
                    counter1++;
                }
                sliceStacks.push_back(multibanded);
                counter2 = 0;
            }
            counter5 += stepFactor;

            chunks_separated.clear();
            chunks_separated_reordered.clear();
        }
    }

    //-------------------------------------------------------------------

    // split stacks into packages based on specific order
    void Reconstruction::splitPackages(const Array<RealImage>& stacks, const Array<int>& pack_num, Array<RealImage>& packageStacks, const Array<int>& order, const int step, const int rewinder) {
        // calculate slice order
        GetSliceAcquisitionOrder(stacks, pack_num, order, step, rewinder);

        // location acquisition order
        Array<int> z_internal_slice_order;
        z_internal_slice_order.reserve(stacks[0].Attributes()._z);

        // dynamic loop
        for (size_t dyn = 0, counter1 = 0; dyn < stacks.size(); dyn++) {
            // current stack
            const RealImage& image = stacks[dyn];
            const ImageAttributes& attr = image.Attributes();
            const int pkg_z = attr._z / pack_num[dyn];

            // slice loop
            for (int sl = 0; sl < attr._z; sl++)
                z_internal_slice_order.push_back(_z_slice_order[counter1 + sl]);

            // package loop
            int counter2 = 0, counter3 = 0;
            for (int p = 0; p < pack_num[dyn]; p++) {
                int internalIterations;
                // slice excess for each package
                if (attr._z - counter2 > pkg_z * pack_num[dyn]) {
                    internalIterations = pkg_z + 1;
                    counter2++;
                } else {
                    internalIterations = pkg_z;
                }

                // copying
                RealImage stack(attr);
                for (int sl = counter3; sl < internalIterations + counter3; sl++)
                    for (int j = 0; j < stack.GetY(); j++)
                        for (int i = 0; i < stack.GetX(); i++)
                            stack.Put(i, j, z_internal_slice_order[sl], image(i, j, z_internal_slice_order[sl]));

                // pushing package
                packageStacks.push_back(move(stack));
                // updating variables for next package
                counter3 += internalIterations;
            }
            counter1 += attr._z;

            z_internal_slice_order.clear();
        }
    }

    //-------------------------------------------------------------------

    // multi-band based
    void Reconstruction::splitPackageswithMB(const Array<RealImage>& stacks, const Array<int>& pack_num, Array<RealImage>& packageStacks, const Array<int>& multiband_vector, const Array<int>& order, const int step, const int rewinder) {
        // initializing variables
        Array<RealImage> chunks, chunksAll, chunks_separated, chunks_separated_reordered;
        Array<int> pack_numAll;
        int counter1, counter2, counter3, counter4;

        // dynamic loop
        for (size_t dyn = 0; dyn < stacks.size(); dyn++) {
            const RealImage& image = stacks[dyn];
            const ImageAttributes& attr = image.Attributes();
            const int& multiband = multiband_vector[dyn];
            const int sliceMB = attr._z / multiband;

            for (int m = 0; m < multiband; m++) {
                RealImage chunk = image.GetRegion(0, 0, m * sliceMB, attr._x, attr._y, (m + 1) * sliceMB);
                chunks.push_back(move(chunk));
                pack_numAll.push_back(pack_num[dyn]);
            }
        }

        // split package
        splitPackages(chunks, pack_numAll, chunksAll, order, step, rewinder);

        counter4 = 0;
        RealImage multibanded;
        // new dynamic loop
        for (size_t dyn = 0; dyn < stacks.size(); dyn++) {
            const RealImage& image = stacks[dyn];
            const int& multiband = multiband_vector[dyn];
            multibanded.Initialize(image.Attributes());

            // getting data from this dynamic
            const int& stepFactor = pack_num[dyn];
            for (int iter = 0; iter < multiband * stepFactor; iter++)
                chunks_separated.push_back(chunksAll[iter + counter4]);
            counter4 += multiband * stepFactor;

            // reordering chunks_separated
            counter1 = 0;
            counter2 = 0;
            counter3 = 0;
            while (counter1 < chunks_separated.size()) {
                chunks_separated_reordered.push_back(chunks_separated[counter2]);

                counter2 += stepFactor;
                if (counter2 > chunks_separated.size() - 1) {
                    counter3++;
                    counter2 = counter3;
                }
                counter1++;
            }

            // reassembling multiband slices
            counter1 = 0;
            counter2 = 0;
            while (counter1 < chunks_separated_reordered.size()) {
                for (int m = 0; m < multiband; m++) {
                    const RealImage& toAdd = chunks_separated_reordered[counter1];
                    for (int k = 0; k < toAdd.GetZ(); k++) {
                        for (int j = 0; j < toAdd.GetY(); j++)
                            for (int i = 0; i < toAdd.GetX(); i++)
                                multibanded.Put(i, j, counter2, toAdd(i, j, k));
                        counter2++;
                    }
                    counter1++;
                }
                packageStacks.push_back(multibanded);
                counter2 = 0;
            }

            chunks_separated.clear();
            chunks_separated_reordered.clear();
        }
    }

    //-------------------------------------------------------------------

    // updated package-to-volume registration
    void Reconstruction::newPackageToVolume(const Array<RealImage>& stacks, const Array<int>& pack_num, const Array<int>& multiband_vector, const Array<int>& order, const int step, const int rewinder, const int iter, const int steps) {
        // copying transformations from previous iterations
        _previous_transformations = _transformations;

        ParameterList params;
        Insert(params, "Transformation model", "Rigid");
        // Insert(params, "Image (dis-)similarity measure", "NMI");
        if (_nmi_bins > 0)
            Insert(params, "No. of bins", _nmi_bins);
        // Insert(params, "Image interpolation mode", "Linear");
        Insert(params, "Background value", -1);
        // Insert(params, "Background value for image 1", -1);
        // Insert(params, "Background value for image 2", -1);

        GenericRegistrationFilter rigidregistration;
        rigidregistration.Parameter(params);

        int wrapper = stacks.size() / steps;
        if (stacks.size() % steps > 0)
            wrapper++;

        GreyImage t;
        Array<RealImage> sstacks, packages;
        Array<int> spack_num, smultiband_vector, sorder, t_internal_slice_order;
        Array<RigidTransformation> internal_transformations;

        for (int w = 0; w < wrapper; w++) {
            const int doffset = w * steps;
            // preparing input for this iterations
            for (int s = 0; s < steps; s++) {
                if (s + doffset < stacks.size()) {
                    sstacks.push_back(stacks[s + doffset]);
                    spack_num.push_back(pack_num[s + doffset]);
                    smultiband_vector.push_back(multiband_vector[s + doffset]);
                    sorder.push_back(order[s + doffset]);
                }
            }

            splitPackageswithMB(sstacks, spack_num, packages, smultiband_vector, sorder, step, rewinder);

            // other variables
            int counter1 = 0, counter2 = 0, counter3 = 0;

            for (size_t i = 0; i < sstacks.size(); i++) {
                const RealImage& firstPackage = packages[counter1];
                const int& multiband = smultiband_vector[i];
                int extra = (firstPackage.GetZ() / multiband) % spack_num[i];
                int startIterations = 0, endIterations = 0;

                // slice loop
                for (int sl = 0; sl < firstPackage.GetZ(); sl++) {
                    t_internal_slice_order.push_back(_t_slice_order[counter3 + sl]);
                    internal_transformations.push_back(_transformations[counter2 + sl]);
                }

                // package look
                for (int j = 0; j < spack_num[i]; j++) {
                    // performing registration
                    const RealImage& target = packages[counter1];
                    const GreyImage& s = _reconstructed;
                    t = target;

                    if (_debug) {
                        t.Write((boost::format("target%1%-%2%-%3%.nii.gz") % iter % (i + doffset) % j).str().c_str());
                        s.Write((boost::format("source%1%-%2%-%3%.nii.gz") % iter % (i + doffset) % j).str().c_str());
                    }

                    //check whether package is empty (all zeros)
                    RealPixel tmin, tmax;
                    target.GetMinMax(&tmin, &tmax);

                    if (tmax > 0) {
                        RigidTransformation offset;
                        ResetOrigin(t, offset);
                        const Matrix& mo = offset.GetMatrix();
                        internal_transformations[j].PutMatrix(internal_transformations[j].GetMatrix() * mo);

                        rigidregistration.Input(&t, &s);
                        Transformation *dofout = nullptr;
                        rigidregistration.Output(&dofout);
                        rigidregistration.InitialGuess(&internal_transformations[j]);
                        rigidregistration.GuessParameter();
                        rigidregistration.Run();

                        unique_ptr<RigidTransformation> rigid_dofout(dynamic_cast<RigidTransformation*>(dofout));
                        internal_transformations[j] = *rigid_dofout;

                        internal_transformations[j].PutMatrix(internal_transformations[j].GetMatrix() * mo.Inverse());
                    }

                    if (_debug)
                        internal_transformations[j].Write((boost::format("transformation%1%-%2%-%3%.dof") % iter % (i + doffset) % j).str().c_str());

                    // saving transformations
                    int iterations = (firstPackage.GetZ() / multiband) / spack_num[i];
                    if (extra > 0) {
                        iterations++;
                        extra--;
                    }
                    endIterations += iterations;

                    for (int k = startIterations; k < endIterations; k++) {
                        for (size_t l = 0; l < t_internal_slice_order.size(); l++) {
                            if (k == t_internal_slice_order[l]) {
                                _transformations[counter2 + l].PutTranslationX(internal_transformations[j].GetTranslationX());
                                _transformations[counter2 + l].PutTranslationY(internal_transformations[j].GetTranslationY());
                                _transformations[counter2 + l].PutTranslationZ(internal_transformations[j].GetTranslationZ());
                                _transformations[counter2 + l].PutRotationX(internal_transformations[j].GetRotationX());
                                _transformations[counter2 + l].PutRotationY(internal_transformations[j].GetRotationY());
                                _transformations[counter2 + l].PutRotationZ(internal_transformations[j].GetRotationZ());
                                _transformations[counter2 + l].UpdateMatrix();
                            }
                        }
                    }
                    startIterations = endIterations;
                    counter1++;
                }
                // resetting variables for next dynamic
                startIterations = 0;
                endIterations = 0;
                counter2 += firstPackage.GetZ();
                counter3 += firstPackage.GetZ();

                t_internal_slice_order.clear();
                internal_transformations.clear();
            }

            //save overall slice order
            const ImageAttributes& attr = stacks[0].Attributes();
            const int slices_per_dyn = attr._z / multiband_vector[0];

            //slice order should repeat for each dynamic - only take first dynamic
            _slice_timing.clear();
            for (size_t dyn = 0; dyn < stacks.size(); dyn++)
                for (int i = 0; i < attr._z; i++) {
                    _slice_timing.push_back(dyn * slices_per_dyn + _t_slice_order[i]);
                    cout << "slice timing = " << _slice_timing[i] << endl;
                }

            for (size_t i = 0; i < _z_slice_order.size(); i++)
                cout << "z(" << i << ")=" << _z_slice_order[i] << endl;

            for (size_t i = 0; i < _t_slice_order.size(); i++)
                cout << "t(" << i << ")=" << _t_slice_order[i] << endl;

            // save transformations and clear
            _z_slice_order.clear();
            _t_slice_order.clear();

            sstacks.clear();
            spack_num.clear();
            smultiband_vector.clear();
            sorder.clear();
            packages.clear();
        }
    }

    //-------------------------------------------------------------------

    // split image into N packages
    void Reconstruction::SplitImage(const RealImage& image, const int packages, Array<RealImage>& stacks) {
        //slices in package
        const int pkg_z = image.Attributes()._z / packages;
        const double pkg_dz = image.Attributes()._dz * packages;

        ClearAndReserve(stacks, packages);

        for (int l = 0; l < packages; l++) {
            ImageAttributes attr = image.Attributes();
            if (pkg_z * packages + l < attr._z)
                attr._z = pkg_z + 1;
            else
                attr._z = pkg_z;
            attr._dz = pkg_dz;

            //fill values in each stack
            RealImage stack(attr);
            double ox, oy, oz;
            stack.GetOrigin(ox, oy, oz);

            for (int k = 0; k < stack.GetZ(); k++)
                for (int j = 0; j < stack.GetY(); j++)
                    for (int i = 0; i < stack.GetX(); i++)
                        stack.Put(i, j, k, image(i, j, k * packages + l));

            //adjust origin
            //original image coordinates
            double x = 0;
            double y = 0;
            double z = l;
            image.ImageToWorld(x, y, z);
            //stack coordinates
            double sx = 0;
            double sy = 0;
            double sz = 0;
            stack.PutOrigin(ox, oy, oz); //adjust to original value
            stack.ImageToWorld(sx, sy, sz);
            //adjust origin
            stack.PutOrigin(ox + (x - sx), oy + (y - sy), oz + (z - sz));
            stacks.push_back(move(stack));
        }
    }

    //-------------------------------------------------------------------

    // split image into 2 packages
    void Reconstruction::SplitImageEvenOdd(const RealImage& image, const int packages, Array<RealImage>& stacks) {
        cout << "Split Image Even Odd: " << packages << " packages." << endl;

        Array<RealImage> packs, packs2;
        SplitImage(image, packages, packs);

        ClearAndReserve(stacks, packs.size() * 2);

        for (size_t i = 0; i < packs.size(); i++) {
            cout << "Package " << i << ": " << endl;
            SplitImage(packs[i], 2, packs2);
            stacks.push_back(move(packs2[0]));
            stacks.push_back(move(packs2[1]));
        }

        cout << "done." << endl;
    }

    //-------------------------------------------------------------------

    // split image into 4 packages
    void Reconstruction::SplitImageEvenOddHalf(const RealImage& image, const int packages, Array<RealImage>& stacks, const int iter) {
        cout << "Split Image Even Odd Half " << iter << endl;

        Array<RealImage> packs, packs2;
        if (iter > 1)
            SplitImageEvenOddHalf(image, packages, packs, iter - 1);
        else
            SplitImageEvenOdd(image, packages, packs);

        ClearAndReserve(stacks, packs.size() * packs2.size());
        for (size_t i = 0; i < packs.size(); i++) {
            HalfImage(packs[i], packs2);
            for (size_t j = 0; j < packs2.size(); j++)
                stacks.push_back(move(packs2[j]));
        }
    }

    //-------------------------------------------------------------------

    // split image into 2 packages
    void Reconstruction::HalfImage(const RealImage& image, Array<RealImage>& stacks) {
        const ImageAttributes& attr = image.Attributes();
        stacks.clear();

        //We would not like single slices - that is reserved for slice-to-volume
        if (attr._z >= 4) {
            stacks.push_back(image.GetRegion(0, 0, 0, attr._x, attr._y, attr._z / 2));
            stacks.push_back(image.GetRegion(0, 0, attr._z / 2, attr._x, attr._y, attr._z));
        } else
            stacks.push_back(image);
    }

    //-------------------------------------------------------------------

    // run package-to-volume registration
    void Reconstruction::PackageToVolume(const Array<RealImage>& stacks, const Array<int>& pack_num, const Array<RigidTransformation>& stack_transformations) {
        SVRTK_START_TIMING();

        int firstSlice = 0;
        Array<int> firstSlice_array;
        firstSlice_array.reserve(stacks.size());

        for (size_t i = 0; i < stacks.size(); i++) {
            firstSlice_array.push_back(firstSlice);
            firstSlice += stacks[i].GetZ();
        }

        ParameterList params;
        Insert(params, "Transformation model", "Rigid");
        // Insert(params, "Image interpolation mode", "Linear");
        Insert(params, "Background value for image 1", 0);
        Insert(params, "Background value for image 2", -1);

        if (_nmi_bins > 0)
            Insert(params, "No. of bins", _nmi_bins);

        GenericRegistrationFilter rigidregistration;
        RealImage mask, target;
        Array<RealImage> packages;

        #pragma omp parallel for private(mask, target, packages, rigidregistration)
        for (size_t i = 0; i < stacks.size(); i++) {
            SplitImage(stacks[i], pack_num[i], packages);
            for (size_t j = 0; j < packages.size(); j++) {
                if (_debug)
                    packages[j].Write((boost::format("package-%1%-%2%.nii.gz") % i % j).str().c_str());

                //packages are not masked at present
                mask = _mask;
                const RigidTransformation& mask_transform = stack_transformations[i]; //s[i];
                TransformMask(packages[j], mask, mask_transform);

                target = packages[j] * mask;
                const RealImage& source = _reconstructed;

                //find existing transformation
                double x = 0, y = 0, z = 0;
                packages[j].ImageToWorld(x, y, z);
                stacks[i].WorldToImage(x, y, z);

                const int firstSliceIndex = round(z) + firstSlice_array[i];
                // cout<<"First slice index for package "<<j<<" of stack "<<i<<" is "<<firstSliceIndex<<endl;

                //put origin in target to zero
                RigidTransformation offset;
                ResetOrigin(target, offset);
                const Matrix& mo = offset.GetMatrix();
                _transformations[firstSliceIndex].PutMatrix(_transformations[firstSliceIndex].GetMatrix() * mo);

                rigidregistration.Parameter(params);
                rigidregistration.Input(&target, &source);
                Transformation *dofout;
                rigidregistration.Output(&dofout);
                rigidregistration.InitialGuess(&_transformations[firstSliceIndex]);
                rigidregistration.GuessParameter();
                rigidregistration.Run();

                unique_ptr<RigidTransformation> rigidTransf(dynamic_cast<RigidTransformation*>(dofout));
                _transformations[firstSliceIndex] = *rigidTransf;

                //undo the offset
                _transformations[firstSliceIndex].PutMatrix(_transformations[firstSliceIndex].GetMatrix() * mo.Inverse());

                if (_debug)
                    _transformations[firstSliceIndex].Write((boost::format("transformation-%1%-%2%.dof") % i % j).str().c_str());

                //set the transformation to all slices of the package
                // cout<<"Slices of the package "<<j<<" of the stack "<<i<<" are: ";
                for (int k = 0; k < packages[j].GetZ(); k++) {
                    x = 0; y = 0; z = k;
                    packages[j].ImageToWorld(x, y, z);
                    stacks[i].WorldToImage(x, y, z);
                    int sliceIndex = round(z) + firstSlice_array[i];
                    // cout<<sliceIndex<<" "<<endl;

                    if (sliceIndex >= _transformations.size()) {
                        cerr << "Reconstruction::PackageToVolume: sliceIndex out of range." << endl;
                        cerr << sliceIndex << " " << _transformations.size() << endl;
                        exit(1);
                    }

                    if (sliceIndex != firstSliceIndex) {
                        _transformations[sliceIndex].PutTranslationX(_transformations[firstSliceIndex].GetTranslationX());
                        _transformations[sliceIndex].PutTranslationY(_transformations[firstSliceIndex].GetTranslationY());
                        _transformations[sliceIndex].PutTranslationZ(_transformations[firstSliceIndex].GetTranslationZ());
                        _transformations[sliceIndex].PutRotationX(_transformations[firstSliceIndex].GetRotationX());
                        _transformations[sliceIndex].PutRotationY(_transformations[firstSliceIndex].GetRotationY());
                        _transformations[sliceIndex].PutRotationZ(_transformations[firstSliceIndex].GetRotationZ());
                        _transformations[sliceIndex].UpdateMatrix();
                    }
                }
            }
            // cout<<"End of stack "<<i<<endl<<endl;
        }

        SVRTK_END_TIMING("PackageToVolume");
    }

    //-------------------------------------------------------------------

    // Crops the image according to the mask
    void Reconstruction::CropImage(RealImage& image, const RealImage& mask) {
        int i, j, k;
        //upper boundary for z coordinate
        for (k = image.GetZ() - 1; k >= 0; k--) {
            int sum = 0;
            for (j = image.GetY() - 1; j >= 0; j--)
                for (i = image.GetX() - 1; i >= 0; i--)
                    if (mask.Get(i, j, k) > 0)
                        sum++;
            if (sum > 0)
                break;
        }
        int z2 = k;

        //lower boundary for z coordinate
        for (k = 0; k <= image.GetZ() - 1; k++) {
            int sum = 0;
            for (j = image.GetY() - 1; j >= 0; j--)
                for (i = image.GetX() - 1; i >= 0; i--)
                    if (mask.Get(i, j, k) > 0)
                        sum++;
            if (sum > 0)
                break;
        }
        int z1 = k;

        //upper boundary for y coordinate
        for (j = image.GetY() - 1; j >= 0; j--) {
            int sum = 0;
            for (k = image.GetZ() - 1; k >= 0; k--)
                for (i = image.GetX() - 1; i >= 0; i--)
                    if (mask.Get(i, j, k) > 0)
                        sum++;
            if (sum > 0)
                break;
        }
        int y2 = j;

        //lower boundary for y coordinate
        for (j = 0; j <= image.GetY() - 1; j++) {
            int sum = 0;
            for (k = image.GetZ() - 1; k >= 0; k--)
                for (i = image.GetX() - 1; i >= 0; i--)
                    if (mask.Get(i, j, k) > 0)
                        sum++;
            if (sum > 0)
                break;
        }
        int y1 = j;

        //upper boundary for x coordinate
        for (i = image.GetX() - 1; i >= 0; i--) {
            int sum = 0;
            for (k = image.GetZ() - 1; k >= 0; k--)
                for (j = image.GetY() - 1; j >= 0; j--)
                    if (mask.Get(i, j, k) > 0)
                        sum++;
            if (sum > 0)
                break;
        }
        int x2 = i;

        //lower boundary for x coordinate
        for (i = 0; i <= image.GetX() - 1; i++) {
            int sum = 0;
            for (k = image.GetZ() - 1; k >= 0; k--)
                for (j = image.GetY() - 1; j >= 0; j--)
                    if (mask.Get(i, j, k) > 0)
                        sum++;
            if (sum > 0)
                break;
        }
        int x1 = i;

        // if no intersection with mask, force exclude
        if ((x2 <= x1) || (y2 <= y1) || (z2 <= z1))
            x1 = y1 = z1 = x2 = y2 = z2 = 0;

        //Cut region of interest
        image = image.GetRegion(x1, y1, z1, x2 + 1, y2 + 1, z2 + 1);
    }

    //-------------------------------------------------------------------

    // GF 190416, useful for handling different slice orders
    void Reconstruction::CropImageIgnoreZ(RealImage& image, const RealImage& mask) {
        int i, j, k;
        //Crops the image according to the mask
        // Filling slices out of mask with zeros
        for (k = image.GetZ() - 1; k >= 0; k--) {
            int sum = 0;
            for (j = image.GetY() - 1; j >= 0; j--)
                for (i = image.GetX() - 1; i >= 0; i--)
                    if (mask.Get(i, j, k) > 0)
                        sum++;
            if (sum > 0) {
                k++;
                break;
            }
        }
        int z2 = k;

        //lower boundary for z coordinate
        for (k = 0; k <= image.GetZ() - 1; k++) {
            int sum = 0;
            for (j = image.GetY() - 1; j >= 0; j--)
                for (i = image.GetX() - 1; i >= 0; i--)
                    if (mask.Get(i, j, k) > 0)
                        sum++;
            if (sum > 0)
                break;
        }
        int z1 = k;

        // Filling upper part
        for (k = z2; k < image.GetZ(); k++)
            for (j = 0; j < image.GetY(); j++)
                for (i = 0; i < image.GetX(); i++)
                    image.Put(i, j, k, 0);

        // Filling lower part
        for (k = 0; k < z1; k++)
            for (j = 0; j < image.GetY(); j++)
                for (i = 0; i < image.GetX(); i++)
                    image.Put(i, j, k, 0);

        //Original ROI
        z1 = 0;
        z2 = image.GetZ() - 1;

        //upper boundary for y coordinate
        for (j = image.GetY() - 1; j >= 0; j--) {
            int sum = 0;
            for (k = image.GetZ() - 1; k >= 0; k--)
                for (i = image.GetX() - 1; i >= 0; i--)
                    if (mask.Get(i, j, k) > 0)
                        sum++;
            if (sum > 0)
                break;
        }
        int y2 = j;

        //lower boundary for y coordinate
        for (j = 0; j <= image.GetY() - 1; j++) {
            int sum = 0;
            for (k = image.GetZ() - 1; k >= 0; k--)
                for (i = image.GetX() - 1; i >= 0; i--)
                    if (mask.Get(i, j, k) > 0)
                        sum++;
            if (sum > 0)
                break;
        }
        int y1 = j;

        //upper boundary for x coordinate
        for (i = image.GetX() - 1; i >= 0; i--) {
            int sum = 0;
            for (k = image.GetZ() - 1; k >= 0; k--)
                for (j = image.GetY() - 1; j >= 0; j--)
                    if (mask.Get(i, j, k) > 0)
                        sum++;
            if (sum > 0)
                break;
        }
        int x2 = i;

        //lower boundary for x coordinate
        for (i = 0; i <= image.GetX() - 1; i++) {
            int sum = 0;
            for (k = image.GetZ() - 1; k >= 0; k--)
                for (j = image.GetY() - 1; j >= 0; j--)
                    if (mask.Get(i, j, k) > 0)
                        sum++;
            if (sum > 0)
                break;
        }
        int x1 = i;

        // if no intersection with mask, force exclude
        if ((x2 <= x1) || (y2 <= y1) || (z2 <= z1))
            x1 = y1 = z1 = x2 = y2 = z2 = 0;

        //Cut region of interest
        image = image.GetRegion(x1, y1, z1, x2 + 1, y2 + 1, z2 + 1);
    }

    //-------------------------------------------------------------------

    // invert transformations
    void Reconstruction::InvertStackTransformations(Array<RigidTransformation>& stack_transformations) {
        //for each stack
        for (size_t i = 0; i < stack_transformations.size(); i++) {
            //invert transformation for the stacks
            stack_transformations[i].Invert();
            stack_transformations[i].UpdateParameter();
        }
    }

    //-------------------------------------------------------------------

    // mask reconstructed volume
    void Reconstruction::MaskVolume() {
        RealPixel *pr = _reconstructed.Data();
        const RealPixel *pm = _mask.Data();
        for (int i = 0; i < _reconstructed.NumberOfVoxels(); i++) {
            if (pm[i] == 0)
                pr[i] = -1;
        }
    }

    //-------------------------------------------------------------------

    // mask input volume
    void Reconstruction::MaskImage(RealImage& image, double padding) {
        if (image.NumberOfVoxels() != _mask.NumberOfVoxels()) {
            cerr << "Cannot mask the image - different dimensions" << endl;
            exit(1);
        }

        RealPixel *pr = image.Data();
        const RealPixel *pm = _mask.Data();
        for (int i = 0; i < image.NumberOfVoxels(); i++) {
            if (pm[i] == 0)
                pr[i] = padding;
        }
    }

    //-------------------------------------------------------------------

    // rescale input volume
    void Reconstruction::Rescale(RealImage& img, double max) {
        // Get lower and upper bound
        RealPixel min_val, max_val;
        img.GetMinMax(&min_val, &max_val);

        RealPixel *ptr = img.Data();
        for (int i = 0; i < img.NumberOfVoxels(); i++) {
            if (ptr[i] > 0)
                ptr[i] = double(ptr[i]) / double(max_val) * max;
        }
    }

    //-------------------------------------------------------------------

    // run stack background filtering (GS based)
    void Reconstruction::BackgroundFiltering(Array<RealImage>& stacks, const double fg_sigma, const double bg_sigma) {
        GaussianBlurring<RealPixel> gb2(stacks[0].GetXSize() * bg_sigma);
        GaussianBlurring<RealPixel> gb3(stacks[0].GetXSize() * fg_sigma);
        RealImage stack, global_blurred, tmp_slice, tmp_slice_b;

        // Do not parallelise: GaussianBlurring has already been parallelised!
        for (size_t j = 0; j < stacks.size(); j++) {
            stack = stacks[j];
            stack.Write((boost::format("original-%1%.nii.gz") % j).str().c_str());

            global_blurred = stacks[j];
            gb2.Input(&global_blurred);
            gb2.Output(&global_blurred);
            gb2.Run();

            // Do not parallelise: GaussianBlurring has already been parallelised!
            for (int i = 0; i < stacks[j].GetZ(); i++) {
                tmp_slice = stacks[j].GetRegion(0, 0, i, stacks[j].GetX(), stacks[j].GetY(), i + 1);
                tmp_slice_b = tmp_slice;

                gb3.Input(&tmp_slice_b);
                gb3.Output(&tmp_slice_b);
                gb3.Run();

                gb2.Input(&tmp_slice);
                gb2.Output(&tmp_slice);
                gb2.Run();

                #pragma omp parallel for
                for (int x = 0; x < stacks[j].GetX(); x++) {
                    for (int y = 0; y < stacks[j].GetY(); y++) {
                        stack(x, y, i) = tmp_slice_b(x, y, 0) + global_blurred(x, y, i) - tmp_slice(x, y, 0);
                        if (stack(x, y, i) < 0)
                            stack(x, y, i) = 1;
                    }
                }
            }

            stack.Write((boost::format("filtered-%1%.nii.gz") % j).str().c_str());

    // another implementation of NCC between images (should be removed or the previous should be replaced)
    double Reconstruction::ComputeNCC(const RealImage& slice_1, const RealImage& slice_2, const double threshold, double *count) {
        const int slice_1_N = slice_1.NumberOfVoxels();
        const int slice_2_N = slice_2.NumberOfVoxels();

        const double *slice_1_ptr = slice_1.Data();
        const double *slice_2_ptr = slice_2.Data();

        int slice_1_n = 0;
        double slice_1_m = 0;
        for (int j = 0; j < slice_1_N; j++) {
            if (slice_1_ptr[j] > threshold && slice_2_ptr[j] > threshold) {
                slice_1_m += slice_1_ptr[j];
                slice_1_n++;
            }
        }
        slice_1_m /= slice_1_n;

        int slice_2_n = 0;
        double slice_2_m = 0;
        for (int j = 0; j < slice_2_N; j++) {
            if (slice_1_ptr[j] > threshold && slice_2_ptr[j] > threshold) {
                slice_2_m += slice_2_ptr[j];
                slice_2_n++;
            }
        }
        slice_2_m /= slice_2_n;

        if (count) {
            *count = 0;
            for (int j = 0; j < slice_1_N; j++)
                if (slice_1_ptr[j] > threshold && slice_2_ptr[j] > threshold)
                    (*count)++;
        }

        double CC_slice;
        if (slice_1_n < 5 || slice_2_n < 5) {
            CC_slice = -1;
        } else {
            double diff_sum = 0;
            double slice_1_sq = 0;
            double slice_2_sq = 0;

            for (int j = 0; j < slice_1_N; j++) {
                if (slice_1_ptr[j] > threshold && slice_2_ptr[j] > threshold) {
                    diff_sum += (slice_1_ptr[j] - slice_1_m) * (slice_2_ptr[j] - slice_2_m);
                    slice_1_sq += pow(slice_1_ptr[j] - slice_1_m, 2);
                    slice_2_sq += pow(slice_2_ptr[j] - slice_2_m, 2);
                }
            }

            if (slice_1_sq * slice_2_sq > 0)
                CC_slice = diff_sum / sqrt(slice_1_sq * slice_2_sq);
            else
                CC_slice = 0;
            stacks[j] = move(stack);
        }

        return CC_slice;
    }

    //-------------------------------------------------------------------

    // run parallel global similarity statists (for the stack selection function)
    void Reconstruction::RunParallelGlobalStackStats(const Array<RealImage>& stacks, const Array<RealImage>& masks, Array<double>& all_global_ncc_array, Array<double>& all_global_volume_array) {
        all_global_ncc_array = Array<double>(stacks.size());
        all_global_volume_array = Array<double>(stacks.size());

        cout << " start ... " << endl;

        Parallel::GlobalSimilarityStats registration(this, stacks.size(), stacks, masks, all_global_ncc_array, all_global_volume_array);
        registration();
    }

    //-------------------------------------------------------------------

    // run serial global similarity statists (for the stack selection function)
    void Reconstruction::GlobalStackStats(RealImage template_stack, const RealImage& template_mask, const Array<RealImage>& stacks, const Array<RealImage>& masks, double& average_ncc, double& average_volume, Array<RigidTransformation>& current_stack_transformations) {
        template_stack *= template_mask;

        RigidTransformation r_init;
        r_init.PutTranslationX(0.0001);
        r_init.PutTranslationY(0.0001);
        r_init.PutTranslationZ(-0.0001);

        ParameterList params;
        Insert(params, "Transformation model", "Rigid");
        Insert(params, "Background value for image 1", 0);
        Insert(params, "Background value for image 2", 0);

        constexpr double source_padding = 0;
        const double target_padding = -inf;
        constexpr bool dofin_invert = false;
        constexpr bool twod = false;
        GenericRegistrationFilter registration;
        GenericLinearInterpolateImageFunction<RealImage> interpolator;
        ImageTransformation imagetransformation;
        imagetransformation.TargetPaddingValue(target_padding);
        imagetransformation.SourcePaddingValue(source_padding);
        imagetransformation.TwoD(twod);
        imagetransformation.Invert(dofin_invert);
        imagetransformation.Interpolator(&interpolator);

        average_ncc = 0;
        average_volume = 0;
        current_stack_transformations.resize(stacks.size());

        #pragma omp parallel for firstprivate(imagetransformation) private(registration) reduction(+: average_ncc, average_volume)
        for (size_t i = 0; i < stacks.size(); i++) {
            RealImage input_stack = stacks[i] * masks[i];

            Transformation *dofout;
            registration.Parameter(params);
            registration.Output(&dofout);
            registration.InitialGuess(&r_init);
            registration.Input(&template_stack, &input_stack);
            registration.GuessParameter();
            registration.Run();
            unique_ptr<RigidTransformation> r_dofout(dynamic_cast<RigidTransformation*>(dofout));
            current_stack_transformations[i] = *r_dofout;

            RealImage output(template_stack.Attributes());
            imagetransformation.Input(&input_stack);
            imagetransformation.Transformation(dofout);
            imagetransformation.Output(&output);
            imagetransformation.Run();
            input_stack = move(output);

            double slice_count = 0;
            const double local_ncc = ComputeNCC(template_stack, input_stack, 0.01, &slice_count);
            average_ncc += local_ncc;
            average_volume += slice_count;
        }

        average_ncc /= stacks.size();
        average_volume /= stacks.size();
        average_volume *= template_stack.GetXSize() * template_stack.GetYSize() * template_stack.GetZSize() / 1000;
    }

    //-------------------------------------------------------------------

    // compute internal stack statistics (volume and inter-slice NCC)
    void Reconstruction::StackStats(RealImage input_stack, const RealImage& mask, double& mask_volume, double& slice_ncc) {
        input_stack *= mask;

        int slice_num = 0;
        for (int z = 0; z < input_stack.GetZ() - 1; z++) {
            constexpr int sh = 1;
            const RealImage slice_1 = input_stack.GetRegion(sh, sh, z, input_stack.GetX() - sh, input_stack.GetY() - sh, z + 1);
            const RealImage slice_2 = input_stack.GetRegion(sh, sh, z + 1, input_stack.GetX() - sh, input_stack.GetY() - sh, z + 2);

            const double local_ncc = ComputeNCC(slice_1, slice_2);
            if (local_ncc > 0) {
                slice_ncc += local_ncc;
                slice_num++;
            }
        }

        if (slice_num > 0)
            slice_ncc /= slice_num;
        else
            slice_ncc = 0;

        int mask_count = 0;
        for (int x = 0; x < mask.GetX(); x++)
            for (int y = 0; y < mask.GetY(); y++)
                for (int z = 0; z < mask.GetZ(); z++)
                    if (mask(x, y, z) > 0.01)
                        mask_count++;

        mask_volume = mask_count * mask.GetXSize() * mask.GetYSize() * mask.GetZSize() / 1000;
    }

    //-------------------------------------------------------------------

} // namespace svrtk
