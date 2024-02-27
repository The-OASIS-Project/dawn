# friday

## Credits

Initial adaptation from the piper project: https://github.com/rhasspy/piper

Piper and the language models are covered under the MIT license.
Vosk Licensed under the Apache License, Version 2.0.

Needed for LUFS calculation: `sudo apt-get install libebur128-dev`

## JARVIS/Friday Installation Notes
### Cmake 3.27.1
1. `tar xvf cmake-3.27.1.tar.gz`
2. `cd cmake-3.27.1`
3. `./configure --system-curl`
4. `make -j8`
5. `sudo make install`

### spdlog
1. `git clone https://github.com/gabime/spdlog.git`
2. `cd spdlog`
3. `mkdir build && cd build`
4. `cmake .. && make -j8`
5. `sudo make install`

### espeak-ng (git)
Before we begin:
`sudo apt purge espeak-ng-data libespeak-ng1 speech-dispatcher-espeak-ng`

1. `git clone https://github.com/rhasspy/espeak-ng.git`
2. `cd espeak-ng`
3. `./autogen.sh`
4. `./configure --prefix=/usr`
5. `make -j8 src/espeak-ng src/speak-ng`
6. `make`
7. `# make docs` - Skip? bash: no: command not found
8. `sudo make LIBDIR=/usr/lib/aarch64-linux-gnu install`

### Onnxruntime (git)
1. `git clone --recursive https://github.com/microsoft/onnxruntime`
2. `cd onnxruntime`
3. `./build.sh --use_cuda --cudnn_home /usr/local/cuda-11.4 --cuda_home  /usr/local/cuda-11.4 --config MinSizeRel --update --build --parallel --build_shared_lib`
4. Needed? - `./build.sh --use_cuda --cudnn_home /usr/local/cuda-11.4 --cuda_home  /usr/local/cuda-11.4 --config MinSizeRel --enable_pybind --parallel --build_wheel`
At this point, one test fails but it doesn't appear to be fatal to us.
`67% tests passed, 1 tests failed out of 3

Total Test time (real) = 342.80 sec

The following tests FAILED:
	  1 - onnxruntime_test_all (Failed)`
5. `sudo cp -a build/Linux/MinSizeRel/libonnxruntime.so* /usr/local/lib/`
6. `sudo mkdir -p /usr/local/include/onnxruntime`
7. `sudo cp include/onnxruntime/core/session/*.h /usr/local/include/onnxruntime`


### piper-phonemize (git)
1. `git clone https://github.com/rhasspy/piper-phonemize.git`
2. `cd piper-phonemize`
2.1. `cd src && cp ../../onnxruntime/include/onnxruntime/core/session/*.h .`
2.2. `cd ..`
3. `mkdir build && cd build`
4. `cmake ..`
5. `make`
6. `sudo make install`
7. Needed? - `make python`

### piper (git)
1. `git clone https://github.com/rhasspy/piper.git`
2. `cd piper`
2.1. `vim src/cpp/CMakeLists.txt`
2.2. Add `/usr/local/include/onnxruntime` and `/usr/local/include/piper-phonemize` to `target_include_directories`
3. `make` - You'll get some errors on copies at the end but it builds.

### kaldi (git) (This is a REALLY long build process!)
1. `sudo apt-get install sox subversion`
2. `sudo git clone -b vosk --single-branch --depth=1 https://github.com/alphacep/kaldi /opt/kaldi`
3. `sudo chown -R $USER /opt/kaldi`
4. `cd /opt/kaldi/tools`
5. Edit Makefile. Remove `-msse -msse2` from `openfst_add_CXXFLAGS`
6. `make openfst cub` (Note: -j# doesn't seem to work here.) *LONG BUILD*
7. `./extras/install_openblas_clapack.sh`
8. `cd ../src`
9. `./configure --mathlib=OPENBLAS_CLAPACK --shared`
10. `make -j 10 online2 lm rnnlm`
11. `cd ../..`
12. `sudo git clone https://github.com/alphacep/vosk-api --depth=1`
13. `sudo chown -R $USER vosk-api`
14. `cd vosk-api/src`
15. `KALDI_ROOT=/opt/kaldi make -j8`
16. `cd ../c`
17. Edit Makefile. Add the following to LDFLAGS: `$(shell pkg-config --libs cuda-11.4 cudart-11.4) -lcusparse -lcublas -lcusolver -lcurand`
17.1. `make`
18. `wget https://alphacephei.com/vosk/models/vosk-model-small-en-us-0.15.zip`
OR
18. THIS ONE -> `wget https://alphacephei.com/vosk/models/vosk-model-en-us-0.22.zip`
18.1. `unzip vosk-model-en-us-0.22.zip`
19. `ln -s vosk-model-en-us-0.22 model`
20. `cp ../python/example/test.wav .`
21. `./test_vosk`

### Copy some files over for compiling
1. `cp -r vosk-model-en-us-0.22 SOURCE_DIR`
2. `cp ../src/vosk_api.h ../src/libvosk.so SOURCE_DIR`
