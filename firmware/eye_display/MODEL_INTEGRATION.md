# Fall Model Integration

The firmware has a camera sampling interface and a clearly labeled frame-difference demo, but it does not include a real TensorFlow Lite Micro model or inference adapter.
The `Samples` value on the `Fall detection` page is the number of decoded frames delivered to the detector; it is not a fall classification result.

## Model requirements

- Format: TensorFlow Lite FlatBuffer with the `TFL3` file identifier
- Input: RGB image with dimensions and quantization matching the inference adapter
- Output: recommended `normal` and `fall` classes
- Suggested filename: `fall_detection.tflite`

## Integration steps

1. Put the exported `.tflite` file in `firmware/eye_display/main/`.
2. Add TensorFlow Lite Micro initialization, input scaling, and output parsing in `fall_detector.c`.
3. Call model validation and interpreter initialization from `fall_detector_load_model()`.
4. Resize each RGB888 frame to the model input dimensions in `fall_detector_process_frame()`.
5. Trigger the screen, buzzer, or network alarm only after the model output passes a confidence threshold.

Until a model and adapter are provided, the firmware remains a DEMO. Motion compares sampled frames; Pose compares the current frame with a 12-sample baseline. Neither value is a probability or a human-pose estimate. Adjustable demo thresholds can trigger a screen-only candidate alert; web results and manual alert tests are explicitly labeled demo/test.

The current `fall_detector_load_model()` only checks the FlatBuffer identifier. The loaded flag does not mean an interpreter has been initialized, and frame processing still returns `ESP_ERR_NOT_SUPPORTED` for inference. Do not report actual AI classification from this placeholder.

See README.md for local controls, event storage, privacy and remaining hardware dependencies. The running platform is ESP-IDF, not OpenVela.
