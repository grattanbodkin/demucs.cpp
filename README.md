# demucs.cpp

Demucs v4 hybrid transformer model reimplemented in C++ with Eigen3

Track 'Zeno - Signs' from MUSDB18-HQ test set

PyTorch CLI inference (output of `demucs /path/to/track` from [this commit of demucs v4](https://github.com/facebookresearch/demucs@2496b8f7f12b01c8dd0187c040000c46e175b44d)):
```
vocals          ==> SDR:   8.264  SIR:  18.353  ISR:  15.794  SAR:   8.303
drums           ==> SDR:  10.111  SIR:  18.503  ISR:  17.089  SAR:  10.746
bass            ==> SDR:   4.222  SIR:  12.615  ISR:   6.973  SAR:   2.974
other           ==> SDR:   7.397  SIR:  11.317  ISR:  14.303  SAR:   8.137
```
PyTorch custom inference in [my script](./scripts/demucs_pytorch_inference.py):
```
vocals          ==> SDR:   8.339  SIR:  18.274  ISR:  15.835  SAR:   8.354
drums           ==> SDR:  10.058  SIR:  18.598  ISR:  17.023  SAR:  10.812
bass            ==> SDR:   3.926  SIR:  12.414  ISR:   6.941  SAR:   3.202
other           ==> SDR:   7.421  SIR:  11.289  ISR:  14.241  SAR:   8.179
```
CPP inference (this codebase):
```
vocals          ==> SDR:   8.339  SIR:  18.276  ISR:  15.836  SAR:   8.346
drums           ==> SDR:  10.058  SIR:  18.596  ISR:  17.019  SAR:  10.810
bass            ==> SDR:   3.919  SIR:  12.436  ISR:   6.931  SAR:   3.182
other           ==> SDR:   7.421  SIR:  11.286  ISR:  14.252  SAR:   8.183
```

*n.b.* for testing purposes in this repo, the random shift in the beginning of the song is fixed to 1337 in both PyTorch and C++.

## Build and run

Out-of-source build with CMake:
```
$ mkdir -p build && cd build && cmake -DCMAKE_BUILD_TYPE=Release ..
$ make
```

The `Release` build type adds optimization flags (Ofast etc.), without which this project is unusably slow.

Run:
```
$ ./demucs.cpp.main ../ggml-demucs/ggml-model-htdemucs-f16.bin ../test/data/gspi_stereo.wav  ./demucs-out-cpp/
```

## Hack

* make lint
* Valgrind memory error test: `valgrind --leak-check=full --show-leak-kinds=all --track-origins=yes --verbose ./demucs.cpp.main ../ggml-demucs/ggml-model-htdemucs-f16.bin ../test/data/gspi_stereo.wav  ./demucs-out-cpp/`
* 
