// cuda runtime 头文件
#include <cuda_runtime.h>
#include <opencv2/opencv.hpp>
#include <stdio.h>
#include <chrono>
#include <fstream>
#include "box.hpp"
#include "utils.hpp"

#define checkRuntime(op) __check_cuda_runtime((op), #op, __FILE__, __LINE__)

bool __check_cuda_runtime(cudaError_t code, const char *op, const char *file, int line)
{

    if (code != cudaSuccess)
    {
        const char *err_name = cudaGetErrorName(code);
        const char *err_message = cudaGetErrorString(code);

        printf("%s:%d  %s failed. \n  code = %s, message = %s\n", file, line, op, err_name, err_message);
        return false;
    }
    return true;
}

std::vector<Box> decode_cpu(float *predict, int rows, int clos, float confidence_threshold = 0.25f, float nms_threshold = 0.45f)
{
    auto systemtime = std::chrono::system_clock::now();
    uint64_t timestamp1(std::chrono::duration_cast<std::chrono::microseconds>(systemtime.time_since_epoch()).count());

    // cx, cy, width, height, objness, classification*80
    // 一行是85列
    std::vector<Box> boxes;
    int num_classes = clos - 5;
    // 第一个循环，根据置信度挑选box
    for (int i = 0; i < rows; i++)
    {
        // 获得每一行的首地址
        float *pitem = predict + i * clos;
        // 获取当前网格有目标的置信度
        float objness = pitem[4];
        if (objness < confidence_threshold)
            continue;

        // 获取类别置信度的首地址
        float *pclass = pitem + 5;
        // std::max_element 返回从pclass到pclass+num_classes中最大值的地址，
        // 减去 pclass 后就是索引
        int label = std::max_element(pclass, pclass + num_classes) - pclass;
        // 分类目标概率
        float prob = pclass[label];
        // 当前网格中有目标，且为某一个类别的的置信度
        float confidence = prob * objness;

        if (confidence < confidence_threshold)
            continue;

        float cx = pitem[0];
        float cy = pitem[1];
        float width = pitem[2];
        float height = pitem[3];
        float left = cx - width * 0.5;
        float top = cy - height * 0.5;
        float right = cx + width * 0.5;
        float bottom = cy + height * 0.5;
        boxes.emplace_back(left, top, right, bottom, confidence, (float)label);
    }
    // 对所有box根据置信度排序
    std::sort(boxes.begin(), boxes.end(), [](Box &a, Box &b)
              { return a.confidence > b.confidence; });
    // 记录box是否被删除，被删除为true
    std::vector<bool> remove_flags(boxes.size());
    // 保存box
    std::vector<Box> box_result;
    box_result.reserve(boxes.size());

    for (int i = 0; i < boxes.size(); i++)
    {
        if (remove_flags[i])
            continue;
        auto &ibox = boxes[i];
        box_result.emplace_back(ibox);
        for (int j = i + 1; j < boxes.size(); ++j)
        {
            if (remove_flags[j])
                continue;
            auto &jbox = boxes[j];
            if (ibox.label == jbox.label)
            {
                if (iou(ibox, jbox) >= nms_threshold)
                    remove_flags[j] = true;
            }
        }
    }
    systemtime = std::chrono::system_clock::now();
    uint64_t timestamp2(std::chrono::duration_cast<std::chrono::microseconds>(systemtime.time_since_epoch()).count());

    printf("cpu yolov5 postprocess %ld ns\n", timestamp2 - timestamp1);

    return box_result;
}
void decode_kernel_invoker(float *predict, int num_bboxes, int num_classes, float confidence_threshold,
                           float nms_threshold, float *invert_affine_matrix, float *parray, int max_objects,
                           int NUM_BOX_ELEMENT, cudaStream_t stream);

std::vector<Box> decode_gpu(float *predict, int rows, int cols,
                            float confidence_threshold = 0.25f,
                            float nms_threshold = 0.45f)
{
    auto systemtime = std::chrono::system_clock::now();
    uint64_t timestamp1(std::chrono::duration_cast<std::chrono::microseconds>(systemtime.time_since_epoch()).count());

    std::vector<Box> box_result;
    cudaStream_t stream = nullptr;
    checkRuntime(cudaStreamCreate(&stream));
    float *predict_device = nullptr;
    float *output_device = nullptr;
    float *output_host = nullptr;
    int max_objects = 100;
    // left, top, right, bottom, confidence, class, keepflag
    int NUM_BOX_ELEMENT = 7;
    checkRuntime(cudaMalloc(&predict_device, rows * cols * sizeof(float)));
    checkRuntime(cudaMalloc(&output_device, sizeof(float) + max_objects * NUM_BOX_ELEMENT * sizeof(float)));
    checkRuntime(cudaMallocHost(&output_host, sizeof(float) + max_objects * NUM_BOX_ELEMENT * sizeof(float)));
    // 异步将host数据cpy到device
    checkRuntime(cudaMemcpyAsync(predict_device, predict, rows * cols * sizeof(float), cudaMemcpyHostToDevice, stream));
    decode_kernel_invoker(
        predict_device, rows, cols - 5, confidence_threshold,
        nms_threshold, nullptr, output_device, max_objects, NUM_BOX_ELEMENT, stream);

    checkRuntime(cudaMemcpyAsync(output_host, output_device,
                                 sizeof(int) + max_objects * NUM_BOX_ELEMENT * sizeof(float),
                                 cudaMemcpyDeviceToHost, stream));

    checkRuntime(cudaStreamSynchronize(stream));

    int num_boxes = std::min((int)output_host[0], max_objects);
    for (int i = 0; i < num_boxes; ++i)
    {
        float *ptr = output_host + 1 + NUM_BOX_ELEMENT * i;
        int keep_flag = ptr[6];
        if (keep_flag)
        {
            box_result.emplace_back(
                ptr[0], ptr[1], ptr[2], ptr[3], ptr[4], (int)ptr[5]);
        }
    }
    checkRuntime(cudaStreamDestroy(stream));
    checkRuntime(cudaFree(predict_device));
    checkRuntime(cudaFree(output_device));
    checkRuntime(cudaFreeHost(output_host));

    systemtime = std::chrono::system_clock::now();
    uint64_t timestamp2(std::chrono::duration_cast<std::chrono::microseconds>(systemtime.time_since_epoch()).count());

    printf("gpu yolov5 postprocess %ld ns\n", timestamp2 - timestamp1);

    return box_result;
}

int main()
{
    std::string root_path("/home/wxf/workspace/tensorrt-tutorial/2.cuda-runtime-api/2.12-yolov5-postprocess/data/");
    std::string data_file = root_path + "predict.data";
    std::string image_file = root_path + "input-image.jpg";
    auto data = load_file(data_file);
    float *ptr = (float *)data.data();
    auto image = cv::imread(image_file);
    int nelem = data.size() / sizeof(float);
    int ncols = 85;
    int nrows = nelem / ncols;
    // auto boxes = decode_cpu(ptr, nrows, ncols);
    auto boxes = decode_gpu(ptr, nrows, ncols);
    for (auto &box : boxes)
    {
        cv::rectangle(image, cv::Point(box.left, box.top), cv::Point(box.right, box.bottom), cv::Scalar(0, 255, 0), 2);
        cv::putText(image, cv::format("%.2f", box.confidence), cv::Point(box.left, box.top - 7), 0, 0.8, cv::Scalar(0, 0, 255), 2, 16);
    }
    std::string save_image_file = root_path + "image-draw.jpg";
    cv::imwrite(save_image_file, image);

    return 0;
}