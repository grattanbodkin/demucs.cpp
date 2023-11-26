#include "crosstransformer.hpp"
#include "dsp.hpp"
#include "encdec.hpp"
#include "layers.hpp"
#include "model.hpp"
#include "tensor.hpp"
#include <Eigen/Dense>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <tuple>
#include <unsupported/Eigen/FFT>
#include <unsupported/Eigen/MatrixFunctions>
#include <vector>

static std::tuple<int, int>
symmetric_zero_padding(Eigen::MatrixXf &padded, const Eigen::MatrixXf &original,
                       int total_padding)
{
    int left_padding = std::floor((float)total_padding / 2.0f);
    int right_padding = total_padding - left_padding;

    int N = original.cols(); // The original number of columns

    // Copy the original mix into the middle of padded_mix
    padded.block(0, left_padding, 2, N) = original;

    // Zero padding on the left
    padded.block(0, 0, 2, left_padding) =
        Eigen::MatrixXf::Zero(2, left_padding);

    // Zero padding on the right
    padded.block(0, N + left_padding, 2, right_padding) =
        Eigen::MatrixXf::Zero(2, right_padding);

    // return left, right padding as tuple
    return std::make_tuple(left_padding, right_padding);
}

// forward declaration of inner fns
static Eigen::Tensor3dXf
shift_inference_4s(struct demucscpp::demucs_model_4s &model,
                   Eigen::MatrixXf &full_audio);

static Eigen::Tensor3dXf
split_inference_4s(struct demucscpp::demucs_model_4s &model,
                   Eigen::MatrixXf &full_audio);

static Eigen::Tensor3dXf
segment_inference_4s(struct demucscpp::demucs_model_4s &model,
                     Eigen::MatrixXf chunk, int segment_sample,
                     struct demucscpp::demucs_segment_buffers_4s &buffers,
                     struct demucscpp::stft_buffers &stft_buf);

Eigen::Tensor3dXf demucscpp::demucs_inference_4s(struct demucs_model_4s &model,
                                                 Eigen::MatrixXf &full_audio)
{
    std::cout << std::fixed << std::setprecision(20) << std::endl;
    demucscppdebug::debug_matrix_xf(full_audio, "full_audio");

    // first, normalize the audio to mean and std
    // ref = wav.mean(0)
    // wav = (wav - ref.mean()) / ref.std()
    // Calculate the overall mean and standard deviation
    // Compute the mean and standard deviation separately for each channel
    Eigen::VectorXf ref_mean_0 = full_audio.colwise().mean();

    float ref_mean = ref_mean_0.mean();
    float ref_std = std::sqrt((ref_mean_0.array() - ref_mean).square().sum() /
                              (ref_mean_0.size() - 1));

    // Normalize the audio
    Eigen::MatrixXf normalized_audio =
        (full_audio.array() - ref_mean) / ref_std;

    demucscppdebug::debug_matrix_xf(normalized_audio, "normalized_audio");

    full_audio = normalized_audio;

    int length = full_audio.cols();
    Eigen::Tensor3dXf waveform_outputs = shift_inference_4s(model, full_audio);

    // now inverse the normalization in Eigen C++
    // sources = sources * ref.std() + ref.mean()
    for (int i = 0; i < 4; ++i)
    {
        for (int j = 0; j < 2; ++j)
        {
            for (int k = 0; k < length; ++k)
            {
                waveform_outputs(i, j, k) =
                    waveform_outputs(i, j, k) * ref_std + ref_mean;
            }
        }
    }

    return waveform_outputs;
}

static Eigen::Tensor3dXf
shift_inference_4s(struct demucscpp::demucs_model_4s &model,
                   Eigen::MatrixXf &full_audio)
{
    demucscppdebug::debug_matrix_xf(full_audio, "mix in shift!");

    // first, apply shifts for time invariance
    // we simply only support shift=1, the demucs default
    // shifts (int): if > 0, will shift in time `mix` by a random amount between
    // 0 and 0.5 sec
    //     and apply the oppositve shift to the output. This is repeated
    //     `shifts` time and all predictions are averaged. This effectively
    //     makes the model time equivariant and improves SDR by up to 0.2
    //     points.
    int max_shift =
        (int)(demucscpp::MAX_SHIFT_SECS * demucscpp::SUPPORTED_SAMPLE_RATE);

    int length = full_audio.cols();

    Eigen::MatrixXf padded_mix(2, length + 2 * max_shift);

    symmetric_zero_padding(padded_mix, full_audio, 2 * max_shift);

    demucscppdebug::debug_matrix_xf(padded_mix, "padded_mix");

    // int offset = rand() % max_shift;
    int offset = 1337;

    std::cout << "1., apply model w/ shift, offset: " << offset << std::endl;

    Eigen::MatrixXf shifted_audio =
        padded_mix.block(0, offset, 2, length + max_shift - offset);

    int shifted_length = shifted_audio.cols();

    demucscppdebug::debug_matrix_xf(shifted_audio, "shifted_audio");

    Eigen::Tensor3dXf waveform_outputs =
        split_inference_4s(model, shifted_audio);

    demucscppdebug::debug_tensor_3dxf(waveform_outputs, "waveform_outputs");

    // trim the output to the original length
    // waveform_outputs = waveform_outputs[..., max_shift:max_shift + length]
    Eigen::Tensor3dXf trimmed_waveform_outputs =
        Eigen::Tensor3dXf(4, 2, length);
    trimmed_waveform_outputs.setZero();

    demucscppdebug::debug_tensor_3dxf(trimmed_waveform_outputs,
                                      "trimmed_waveform_outputs zero");

    std::cout << "trimming max_shift: " << max_shift << std::endl;
    std::cout << "or trimming offset?: " << offset << std::endl;

    std::cout << "size diff between outputs and trimmed: "
              << trimmed_waveform_outputs.dimension(2) -
                     waveform_outputs.dimension(2)
              << std::endl;

    for (int i = 0; i < 4; ++i)
    {
        for (int j = 0; j < 2; ++j)
        {
            for (int k = 0; k < length; ++k)
            {
                // offset or max shift here? confusing...
                trimmed_waveform_outputs(i, j, k) =
                    waveform_outputs(i, j, k + max_shift - offset);
            }
        }
    }

    return trimmed_waveform_outputs;
}

static Eigen::Tensor3dXf
split_inference_4s(struct demucscpp::demucs_model_4s &model,
                   Eigen::MatrixXf &full_audio)
{
    std::cout << "in split inference!" << std::endl;

    demucscppdebug::debug_matrix_xf(full_audio, "full_audio");

    // calculate segment in samples
    int segment_samples =
        (int)(demucscpp::SEGMENT_LEN_SECS * demucscpp::SUPPORTED_SAMPLE_RATE);

    // let's create reusable buffers with padded sizes
    struct demucscpp::demucs_segment_buffers_4s buffers(2, segment_samples);
    struct demucscpp::stft_buffers stft_buf(buffers.padded_segment_samples);

    // next, use splits with weighted transition and overlap
    // split (bool): if True, the input will be broken down in 8 seconds
    // extracts
    //     and predictions will be performed individually on each and
    //     concatenated. Useful for model with large memory footprint like
    //     Tasnet.

    int stride_samples = (int)((1 - demucscpp::OVERLAP) * segment_samples);

    int length = full_audio.cols();

    // create an output tensor of zeros for four source waveforms
    Eigen::Tensor3dXf out = Eigen::Tensor3dXf(4, 2, length);
    out.setZero();

    // create weight tensor
    Eigen::VectorXf weight(segment_samples);
    weight.setZero();

    for (int i = 0; i < segment_samples / 2; ++i)
    {
        weight(i) = i + 1;
        weight(segment_samples - i - 1) = i + 1;
    }
    weight /= weight.maxCoeff();
    weight = weight.array().pow(demucscpp::TRANSITION_POWER);

    Eigen::VectorXf sum_weight(length);
    sum_weight.setZero();

    // for loop from 0 to length with stride stride_samples

    /* ------------------------------------------------------*/
    /* ----> SERIOUS PARALLELISM CAN BE ACHIEVED HERE?? <----*/
    /* ------------------------------------------------------*/
    for (int offset = 0; offset < length; offset += stride_samples)
    {
        // create a chunk of the padded_full_audio
        int chunk_end = std::min(segment_samples, length - offset);
        Eigen::MatrixXf chunk = full_audio.block(0, offset, 2, chunk_end);
        int chunk_length = chunk.cols();

        std::cout << "2., apply model w/ split, offset: " << offset
                  << ", chunk shape: (" << chunk.rows() << ", " << chunk.cols()
                  << ")" << std::endl;

        Eigen::Tensor3dXf chunk_out = segment_inference_4s(
            model, chunk, segment_samples, buffers, stft_buf);

        demucscppdebug::debug_tensor_3dxf(
            chunk_out, "chunk_out for offset: " + std::to_string(offset));

        // add the weighted chunk to the output
        // out[..., offset:offset + segment] += (weight[:chunk_length] *
        // chunk_out).to(mix.device)
        for (int i = 0; i < 4; ++i)
        {
            for (int j = 0; j < 2; ++j)
            {
                for (int k = 0; k < chunk_length; ++k)
                {
                    if (offset + k >= length)
                    {
                        break;
                    }
                    out(i, j, offset + k) +=
                        weight(k % chunk_length) * chunk_out(i, j, k);
                }
            }
        }

        // sum_weight[offset:offset + segment] +=
        // weight[:chunk_length].to(mix.device)
        for (int k = 0; k < chunk_length; ++k)
        {
            if (offset + k >= length)
            {
                break;
            }
            sum_weight(offset + k) += weight(k % chunk_length);
        }
    }

    demucscppdebug::assert_(sum_weight.minCoeff() > 0);

    for (int i = 0; i < 4; ++i)
    {
        for (int j = 0; j < 2; ++j)
        {
            for (int k = 0; k < length; ++k)
            {
                out(i, j, k) /= sum_weight[k];
            }
        }
    }

    demucscppdebug::debug_tensor_3dxf(out, "out");
    return out;

    // now copy the appropriate segment of the output
    // into the output tensor same shape as the input
    // Eigen::Tensor3dXf waveform_outputs(4, 2, length);

    // for (int i = 0; i < 4; ++i) {
    //     for (int j = 0; j < 2; ++j) {
    //         for (int k = 0; k < length; ++k) {
    //             waveform_outputs(i, j, k) = out(i, j, k);
    //         }
    //     }
    // }
    // demucscppdebug::debug_tensor_3dxf(waveform_outputs, "waveform_outputs");

    // return waveform_outputs;
}

static Eigen::Tensor3dXf
segment_inference_4s(struct demucscpp::demucs_model_4s &model,
                     Eigen::MatrixXf chunk, int segment_samples,
                     struct demucscpp::demucs_segment_buffers_4s &buffers,
                     struct demucscpp::stft_buffers &stft_buf)
{
    std::cout << "in segment inference!" << std::endl;

    demucscppdebug::debug_matrix_xf(chunk, "chunk");

    int chunk_length = chunk.cols();

    // copy chunk into buffers.mix with symmetric zero-padding
    // assign two ints to tuple return value
    std::tuple<int, int> padding = symmetric_zero_padding(
        buffers.mix, chunk, segment_samples - chunk_length);

    // apply demucs inference
    demucscpp::model_inference_4s(model, buffers, stft_buf);

    // copy from buffers.targets_out into chunk_out with center trimming
    Eigen::Tensor3dXf chunk_out = Eigen::Tensor3dXf(4, 2, chunk_length);
    chunk_out.setZero();

    demucscppdebug::debug_tensor_3dxf(chunk_out, "chunk_out");
    demucscppdebug::debug_tensor_3dxf(buffers.targets_out,
                                      "buffers.targets_out");

    std::cout << "padding offset is: " << std::get<0>(padding) << std::endl;

    for (int i = 0; i < 4; ++i)
    {
        for (int j = 0; j < 2; ++j)
        {
            for (int k = 0; k < chunk_length; ++k)
            {
                // undoing center_trim
                chunk_out(i, j, k) =
                    buffers.targets_out(i, j, k + std::get<0>(padding));
            }
        }
    }

    demucscppdebug::debug_tensor_3dxf(chunk_out, "chunk_out");

    return chunk_out;
}
