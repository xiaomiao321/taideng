#include <jni.h>

#include <android/log.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "rknn_api.h"

namespace {

constexpr char kLogTag[] = "RknnPoseNative";
constexpr float kPersonThreshold = 0.50F;
constexpr int kKeypointCount = 17;
constexpr int kKeypointValues = 3;
constexpr int kDetectionOutputCount = 3;
constexpr uint8_t kLetterboxColor = 114;

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, kLogTag, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, kLogTag, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, kLogTag, __VA_ARGS__)

struct Letterbox {
    float scale = 1.0F;
    int pad_x = 0;
    int pad_y = 0;
};

struct PoseContext {
    rknn_context rknn = 0;
    rknn_input_output_num io_num{};
    std::vector<rknn_tensor_attr> input_attrs;
    std::vector<rknn_tensor_attr> output_attrs;
    std::vector<uint8_t> rgb_input;
    int model_width = 0;
    int model_height = 0;
    int model_channels = 0;
    std::mutex mutex;
};

void throw_java(JNIEnv* env, const char* class_name, const std::string& message) {
    jclass exception_class = env->FindClass(class_name);
    if (exception_class != nullptr) {
        env->ThrowNew(exception_class, message.c_str());
    }
}

std::string dims_to_string(const rknn_tensor_attr& attr) {
    std::string result = "[";
    for (uint32_t i = 0; i < attr.n_dims; ++i) {
        if (i > 0) result += ",";
        result += std::to_string(attr.dims[i]);
    }
    result += "]";
    return result;
}

void log_tensor(const char* kind, const rknn_tensor_attr& attr) {
    LOGI(
        "%s[%u] %s dims=%s fmt=%s type=%s qnt=%s zp=%d scale=%g",
        kind,
        attr.index,
        attr.name,
        dims_to_string(attr).c_str(),
        get_format_string(attr.fmt),
        get_type_string(attr.type),
        get_qnt_type_string(attr.qnt_type),
        attr.zp,
        attr.scale
    );
}

inline uint8_t clip_byte(int value) {
    return static_cast<uint8_t>(std::clamp(value, 0, 255));
}

void nv12_to_letterboxed_rgb(
    const uint8_t* nv12,
    int source_width,
    int source_height,
    PoseContext& context,
    Letterbox& letterbox
) {
    const float width_scale = static_cast<float>(context.model_width) / source_width;
    const float height_scale = static_cast<float>(context.model_height) / source_height;
    letterbox.scale = std::min(width_scale, height_scale);

    const int scaled_width = std::max(1, static_cast<int>(std::round(source_width * letterbox.scale)));
    const int scaled_height = std::max(1, static_cast<int>(std::round(source_height * letterbox.scale)));
    letterbox.pad_x = (context.model_width - scaled_width) / 2;
    letterbox.pad_y = (context.model_height - scaled_height) / 2;

    const size_t rgb_size = static_cast<size_t>(context.model_width) *
                            context.model_height * context.model_channels;
    context.rgb_input.assign(rgb_size, kLetterboxColor);

    const size_t uv_plane_offset = static_cast<size_t>(source_width) * source_height;
    for (int output_y = 0; output_y < scaled_height; ++output_y) {
        const int source_y = std::min(source_height - 1, output_y * source_height / scaled_height);
        const int model_y = output_y + letterbox.pad_y;

        for (int output_x = 0; output_x < scaled_width; ++output_x) {
            const int source_x = std::min(source_width - 1, output_x * source_width / scaled_width);
            const int model_x = output_x + letterbox.pad_x;

            const int y_value = nv12[source_y * source_width + source_x];
            const size_t uv_index = uv_plane_offset +
                                    static_cast<size_t>(source_y / 2) * source_width +
                                    static_cast<size_t>(source_x & ~1);
            const int u_value = nv12[uv_index];
            const int v_value = nv12[uv_index + 1];

            const int c = std::max(0, y_value - 16);
            const int d = u_value - 128;
            const int e = v_value - 128;
            const uint8_t red = clip_byte((298 * c + 409 * e + 128) >> 8);
            const uint8_t green = clip_byte((298 * c - 100 * d - 208 * e + 128) >> 8);
            const uint8_t blue = clip_byte((298 * c + 516 * d + 128) >> 8);

            const size_t rgb_index = (static_cast<size_t>(model_y) * context.model_width + model_x) * 3;
            context.rgb_input[rgb_index] = red;
            context.rgb_input[rgb_index + 1] = green;
            context.rgb_input[rgb_index + 2] = blue;
        }
    }
}

float half_to_float(uint16_t half) {
    const uint32_t sign = static_cast<uint32_t>(half & 0x8000U) << 16U;
    int32_t exponent = (half >> 10U) & 0x1FU;
    uint32_t mantissa = half & 0x03FFU;
    uint32_t value = 0;

    if (exponent == 0) {
        if (mantissa == 0) {
            value = sign;
        } else {
            exponent = 1;
            while ((mantissa & 0x0400U) == 0) {
                mantissa <<= 1U;
                --exponent;
            }
            mantissa &= 0x03FFU;
            const uint32_t float_exponent = static_cast<uint32_t>(exponent + (127 - 15));
            value = sign | (float_exponent << 23U) | (mantissa << 13U);
        }
    } else if (exponent == 0x1FU) {
        value = sign | 0x7F800000U | (mantissa << 13U);
    } else {
        const uint32_t float_exponent = static_cast<uint32_t>(exponent + (127 - 15));
        value = sign | (float_exponent << 23U) | (mantissa << 13U);
    }

    float result;
    std::memcpy(&result, &value, sizeof(result));
    return result;
}

float read_tensor_value(const rknn_tensor_attr& attr, const void* data, size_t index) {
    switch (attr.type) {
        case RKNN_TENSOR_INT8:
            return (static_cast<const int8_t*>(data)[index] - static_cast<float>(attr.zp)) * attr.scale;
        case RKNN_TENSOR_UINT8:
            return (static_cast<const uint8_t*>(data)[index] - static_cast<float>(attr.zp)) * attr.scale;
        case RKNN_TENSOR_FLOAT16:
            return half_to_float(static_cast<const uint16_t*>(data)[index]);
        case RKNN_TENSOR_FLOAT32:
            return static_cast<const float*>(data)[index];
        default:
            return 0.0F;
    }
}

inline float sigmoid(float value) {
    return 1.0F / (1.0F + std::exp(-value));
}

bool find_best_person(
    const PoseContext& context,
    const std::vector<rknn_output>& outputs,
    int& best_keypoint_index,
    float& best_score
) {
    best_keypoint_index = -1;
    best_score = kPersonThreshold;
    int anchor_offset = 0;

    for (int output_index = 0; output_index < kDetectionOutputCount; ++output_index) {
        const rknn_tensor_attr& attr = context.output_attrs[output_index];
        if (attr.n_dims != 4 || attr.fmt != RKNN_TENSOR_NCHW) {
            LOGE("Unexpected detection tensor %d layout: %s %s", output_index,
                 dims_to_string(attr).c_str(), get_format_string(attr.fmt));
            return false;
        }

        const int grid_height = static_cast<int>(attr.dims[2]);
        const int grid_width = static_cast<int>(attr.dims[3]);
        const int grid_area = grid_height * grid_width;
        if (attr.dims[1] < 65 || attr.n_elems < static_cast<uint32_t>(65 * grid_area)) {
            LOGE("Detection tensor %d is too small", output_index);
            return false;
        }

        const size_t class_offset = static_cast<size_t>(64) * grid_area;
        for (int cell = 0; cell < grid_area; ++cell) {
            const float score = sigmoid(read_tensor_value(attr, outputs[output_index].buf, class_offset + cell));
            if (score > best_score) {
                best_score = score;
                best_keypoint_index = anchor_offset + cell;
            }
        }
        anchor_offset += grid_area;
    }
    return best_keypoint_index >= 0;
}

std::vector<float> decode_pose(
    const PoseContext& context,
    const std::vector<rknn_output>& outputs,
    const Letterbox& letterbox,
    int source_width,
    int source_height,
    float inference_time_ms
) {
    int keypoint_index = -1;
    float person_score = 0.0F;
    if (!find_best_person(context, outputs, keypoint_index, person_score)) {
        return {inference_time_ms, 0.0F};
    }

    const rknn_tensor_attr& keypoint_attr = context.output_attrs[3];
    const int keypoint_channels = kKeypointCount * kKeypointValues;
    if (keypoint_attr.n_elems % keypoint_channels != 0) {
        LOGE("Unexpected keypoint tensor size: %u", keypoint_attr.n_elems);
        return {inference_time_ms, 0.0F};
    }
    const int anchor_count = static_cast<int>(keypoint_attr.n_elems / keypoint_channels);
    if (keypoint_index >= anchor_count) {
        LOGE("Keypoint index %d is outside %d anchors", keypoint_index, anchor_count);
        return {inference_time_ms, 0.0F};
    }

    std::vector<float> result(2 + keypoint_channels);
    result[0] = inference_time_ms;
    result[1] = person_score;
    for (int keypoint = 0; keypoint < kKeypointCount; ++keypoint) {
        const size_t channel_base = static_cast<size_t>(keypoint * kKeypointValues) * anchor_count;
        const float model_x = read_tensor_value(
            keypoint_attr, outputs[3].buf, channel_base + keypoint_index
        );
        const float model_y = read_tensor_value(
            keypoint_attr, outputs[3].buf, channel_base + anchor_count + keypoint_index
        );
        const float confidence = read_tensor_value(
            keypoint_attr, outputs[3].buf, channel_base + 2 * anchor_count + keypoint_index
        );

        const float source_x = (model_x - letterbox.pad_x) / letterbox.scale;
        const float source_y = (model_y - letterbox.pad_y) / letterbox.scale;
        const size_t result_offset = 2 + static_cast<size_t>(keypoint) * kKeypointValues;
        result[result_offset] = std::clamp(source_x / source_width, 0.0F, 1.0F);
        result[result_offset + 1] = std::clamp(source_y / source_height, 0.0F, 1.0F);
        result[result_offset + 2] = std::clamp(confidence, 0.0F, 1.0F);
    }
    return result;
}

bool initialize_context(PoseContext& context, void* model_data, uint32_t model_size, std::string& error) {
    int status = rknn_init(&context.rknn, model_data, model_size, 0, nullptr);
    if (status != RKNN_SUCC) {
        error = "rknn_init failed: " + std::to_string(status);
        return false;
    }

    rknn_sdk_version sdk_version{};
    if (rknn_query(context.rknn, RKNN_QUERY_SDK_VERSION, &sdk_version, sizeof(sdk_version)) == RKNN_SUCC) {
        LOGI("RKNN API %s, driver %s", sdk_version.api_version, sdk_version.drv_version);
    }

    // RK3566 has a single NPU core. Core masks are only supported on multi-core
    // platforms, so leave scheduling to the runtime shared by pose and ASR.
    LOGI("RK3566 single-core NPU: pose uses runtime default scheduling");

    status = rknn_query(context.rknn, RKNN_QUERY_IN_OUT_NUM, &context.io_num, sizeof(context.io_num));
    if (status != RKNN_SUCC || context.io_num.n_input != 1 || context.io_num.n_output < 4) {
        error = "Unexpected RKNN input/output count";
        return false;
    }

    context.input_attrs.resize(context.io_num.n_input);
    for (uint32_t index = 0; index < context.io_num.n_input; ++index) {
        context.input_attrs[index].index = index;
        status = rknn_query(
            context.rknn, RKNN_QUERY_INPUT_ATTR, &context.input_attrs[index], sizeof(rknn_tensor_attr)
        );
        if (status != RKNN_SUCC) {
            error = "Failed to query RKNN input tensor";
            return false;
        }
        log_tensor("input", context.input_attrs[index]);
    }

    context.output_attrs.resize(context.io_num.n_output);
    for (uint32_t index = 0; index < context.io_num.n_output; ++index) {
        context.output_attrs[index].index = index;
        status = rknn_query(
            context.rknn, RKNN_QUERY_OUTPUT_ATTR, &context.output_attrs[index], sizeof(rknn_tensor_attr)
        );
        if (status != RKNN_SUCC) {
            error = "Failed to query RKNN output tensor";
            return false;
        }
        log_tensor("output", context.output_attrs[index]);
    }

    const rknn_tensor_attr& input = context.input_attrs[0];
    if (input.n_dims != 4) {
        error = "RKNN input tensor is not four-dimensional";
        return false;
    }
    if (input.fmt == RKNN_TENSOR_NCHW) {
        context.model_channels = static_cast<int>(input.dims[1]);
        context.model_height = static_cast<int>(input.dims[2]);
        context.model_width = static_cast<int>(input.dims[3]);
    } else {
        context.model_height = static_cast<int>(input.dims[1]);
        context.model_width = static_cast<int>(input.dims[2]);
        context.model_channels = static_cast<int>(input.dims[3]);
    }
    if (context.model_channels != 3) {
        error = "RKNN model does not have a three-channel input";
        return false;
    }
    return true;
}

}  // namespace

extern "C" JNIEXPORT jlong JNICALL
Java_com_example_rk3566hardwarevalidator_hardware_pose_RknnPoseEngine_nativeCreate(
    JNIEnv* env,
    jobject,
    jbyteArray model_data
) {
    if (model_data == nullptr) {
        throw_java(env, "java/lang/IllegalArgumentException", "RKNN model data is null");
        return 0;
    }

    const jsize model_size = env->GetArrayLength(model_data);
    jbyte* model_bytes = env->GetByteArrayElements(model_data, nullptr);
    if (model_bytes == nullptr) {
        throw_java(env, "java/lang/OutOfMemoryError", "Unable to access RKNN model data");
        return 0;
    }

    auto context = std::make_unique<PoseContext>();
    std::string error;
    const bool initialized = initialize_context(
        *context, model_bytes, static_cast<uint32_t>(model_size), error
    );
    env->ReleaseByteArrayElements(model_data, model_bytes, JNI_ABORT);

    if (!initialized) {
        if (context->rknn != 0) rknn_destroy(context->rknn);
        LOGE("%s", error.c_str());
        throw_java(env, "java/lang/IllegalStateException", error);
        return 0;
    }

    LOGI("RKNN pose model initialized: %dx%dx%d", context->model_width,
         context->model_height, context->model_channels);
    return static_cast<jlong>(reinterpret_cast<intptr_t>(context.release()));
}

extern "C" JNIEXPORT jfloatArray JNICALL
Java_com_example_rk3566hardwarevalidator_hardware_pose_RknnPoseEngine_nativeDetect(
    JNIEnv* env,
    jobject,
    jlong handle,
    jobject nv12_buffer,
    jint source_width,
    jint source_height
) {
    auto* context = reinterpret_cast<PoseContext*>(static_cast<intptr_t>(handle));
    if (context == nullptr || nv12_buffer == nullptr) {
        throw_java(env, "java/lang/IllegalStateException", "RKNN pose engine is not initialized");
        return nullptr;
    }
    if (source_width <= 0 || source_height <= 0 || (source_width & 1) != 0 || (source_height & 1) != 0) {
        throw_java(env, "java/lang/IllegalArgumentException", "NV12 frame dimensions must be positive and even");
        return nullptr;
    }

    auto* nv12 = static_cast<uint8_t*>(env->GetDirectBufferAddress(nv12_buffer));
    const jlong capacity = env->GetDirectBufferCapacity(nv12_buffer);
    const jlong required_size = static_cast<jlong>(source_width) * source_height * 3 / 2;
    if (nv12 == nullptr || capacity < required_size) {
        throw_java(env, "java/lang/IllegalArgumentException", "Pose frame must use a sufficiently large direct buffer");
        return nullptr;
    }

    std::lock_guard<std::mutex> lock(context->mutex);
    Letterbox letterbox;
    nv12_to_letterboxed_rgb(nv12, source_width, source_height, *context, letterbox);

    rknn_input input{};
    input.index = 0;
    input.buf = context->rgb_input.data();
    input.size = static_cast<uint32_t>(context->rgb_input.size());
    input.type = RKNN_TENSOR_UINT8;
    input.fmt = RKNN_TENSOR_NHWC;
    input.pass_through = 0;

    int status = rknn_inputs_set(context->rknn, 1, &input);
    if (status != RKNN_SUCC) {
        throw_java(env, "java/lang/IllegalStateException", "rknn_inputs_set failed: " + std::to_string(status));
        return nullptr;
    }

    const auto start = std::chrono::steady_clock::now();
    status = rknn_run(context->rknn, nullptr);
    const auto end = std::chrono::steady_clock::now();
    if (status != RKNN_SUCC) {
        throw_java(env, "java/lang/IllegalStateException", "rknn_run failed: " + std::to_string(status));
        return nullptr;
    }
    const float inference_time_ms =
        std::chrono::duration<float, std::milli>(end - start).count();

    std::vector<rknn_output> outputs(context->io_num.n_output);
    for (uint32_t index = 0; index < context->io_num.n_output; ++index) {
        outputs[index].index = index;
        outputs[index].want_float = 0;
        outputs[index].is_prealloc = 0;
    }
    status = rknn_outputs_get(context->rknn, context->io_num.n_output, outputs.data(), nullptr);
    if (status != RKNN_SUCC) {
        throw_java(env, "java/lang/IllegalStateException", "rknn_outputs_get failed: " + std::to_string(status));
        return nullptr;
    }

    const std::vector<float> decoded = decode_pose(
        *context, outputs, letterbox, source_width, source_height, inference_time_ms
    );
    rknn_outputs_release(context->rknn, context->io_num.n_output, outputs.data());

    jfloatArray result = env->NewFloatArray(static_cast<jsize>(decoded.size()));
    if (result != nullptr) {
        env->SetFloatArrayRegion(result, 0, static_cast<jsize>(decoded.size()), decoded.data());
    }
    return result;
}

extern "C" JNIEXPORT void JNICALL
Java_com_example_rk3566hardwarevalidator_hardware_pose_RknnPoseEngine_nativeDestroy(
    JNIEnv*,
    jobject,
    jlong handle
) {
    auto* context = reinterpret_cast<PoseContext*>(static_cast<intptr_t>(handle));
    if (context == nullptr) return;
    {
        std::lock_guard<std::mutex> lock(context->mutex);
        if (context->rknn != 0) {
            rknn_destroy(context->rknn);
            context->rknn = 0;
        }
    }
    delete context;
}
