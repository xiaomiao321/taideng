# RK3566 streaming ASR model

- Active model: `sherpa-onnx-streaming-zipformer-small-bilingual-zh-en-rknn-2023-02-16`
- Source: https://github.com/k2-fsa/sherpa-onnx/releases/tag/asr-models
- Source archive: `sherpa-onnx-streaming-zipformer-small-bilingual-zh-en-2023-02-16.tar.bz2`
- Upstream checkpoint: https://huggingface.co/csukuangfj/k2fsa-zipformer-chinese-english-mixed
- Runtime: sherpa-onnx `1.13.6` with packaged RKNN Runtime `2.3.2`
- Conversion: RKNN-Toolkit2 `2.3.2`, target `rk3566`, FP16
- Scheduling: runtime default scheduling on the RK3566 single-core NPU
- Converted: 2026-08-25

Packaged model hashes:

- `encoder.rknn`: `3A77C6279BEF04D2C32D23E250DCAD3D02BC6EC5DEDF6EA22F0EE617467306BC`
- `decoder.rknn`: `0FFBCC11030A8EA7EF9C94EFFC87E244D27E05B5540AD186ADC389417D1FD366`
- `joiner.rknn`: `2B7055FB06CBCC7E8E45CC8BB9B37CA26F624349B5CF7419E37B37011D9B8B74`
- `tokens.txt`: `A8E0E4EC53810E433789B54A5C0134A7EAA2FFCA595A6334D54C00DA858841D3`

The three RKNN graphs were rebuilt from the official FP32 ONNX files for RK3566. The app uses one serial recognition stream and shares the board's only NPU core with pose inference. On-board initialization has passed with RKNPU driver `0.8.8`; speech accuracy and RTF still require a controlled audio test.

The model repository does not currently provide model-card license metadata. Verify the model and training-data license before commercial distribution.
