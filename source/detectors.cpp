// Copyright (C) 2018 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#include <gflags/gflags.h>
#include <functional>
#include <iostream>
#include <fstream>
#include <random>
#include <memory>
#include <chrono>
#include <vector>
#include <string>
#include <utility>
#include <algorithm>
#include <iterator>
#include <map>

#include <inference_engine.hpp>

#include <samples/ocv_common.hpp>
#include <samples/slog.hpp>

#include <ie_iextension.h>
#include <ext_list.hpp>

#include "detectors.hpp"

using namespace InferenceEngine;

BaseDetection::BaseDetection(std::string topoName,
                             const std::string &pathToModel,
                             const std::string &deviceForInference,
                             int maxBatch, bool isBatchDynamic, bool isAsync)
    : topoName(topoName), pathToModel(pathToModel), deviceForInference(deviceForInference),
      maxBatch(maxBatch), isBatchDynamic(isBatchDynamic), isAsync(isAsync),
      enablingChecked(false), _enabled(false) {
    if (isAsync) {
        slog::info << "Use async mode for " << topoName << slog::endl;
    }
}

BaseDetection::~BaseDetection() {}

ExecutableNetwork* BaseDetection::operator ->() {
    return &net;
}

void BaseDetection::submitRequest() {
    if (!enabled() || request == nullptr) return;
    if (isAsync) {
        request->StartAsync();
    } else {
        request->Infer();
    }
}

void BaseDetection::wait() {
    if (!enabled()|| !request || !isAsync)
        return;
    request->Wait(IInferRequest::WaitMode::RESULT_READY);
}

bool BaseDetection::enabled() const  {
    if (!enablingChecked) {
        _enabled = !pathToModel.empty();
        if (!_enabled) {
            slog::info << topoName << " DISABLED" << slog::endl;
        }
        enablingChecked = true;
    }
    return _enabled;
}

void BaseDetection::printPerformanceCounts() {
    if (!enabled()) {
        return;
    }
    slog::info << "Performance counts for " << topoName << slog::endl << slog::endl;
    ::printPerformanceCounts(request->GetPerformanceCounts(), std::cout, false);
}


FaceDetection::FaceDetection(const std::string &pathToModel,
                             const std::string &deviceForInference,
                             int maxBatch, bool isBatchDynamic, bool isAsync,
                             double detectionThreshold, bool doRawOutputMessages)
    : BaseDetection("Face Detection", pathToModel, deviceForInference, maxBatch, isBatchDynamic, isAsync),
      detectionThreshold(detectionThreshold), doRawOutputMessages(doRawOutputMessages),
      enquedFrames(0), width(0), height(0), bb_enlarge_coefficient(1.2), resultsFetched(false) {
}

void FaceDetection::submitRequest() {
    if (!enquedFrames) return;
    enquedFrames = 0;
    resultsFetched = false;
    results.clear();
    BaseDetection::submitRequest();
}

void FaceDetection::enqueue(const cv::Mat &frame) {
    if (!enabled()) return;

    if (!request) {
        request = net.CreateInferRequestPtr();
    }

    width = frame.cols;
    height = frame.rows;

    Blob::Ptr  inputBlob = request->GetBlob(input);

    matU8ToBlob<uint8_t>(frame, inputBlob);

    enquedFrames = 1;
}

CNNNetwork FaceDetection::read()  {
    slog::info << "Loading network files for Face Detection" << slog::endl;
    CNNNetReader netReader;
    /** Read network model **/
    netReader.ReadNetwork(pathToModel);
    /** Set batch size to 1 **/
    slog::info << "Batch size is set to " << maxBatch << slog::endl;
    netReader.getNetwork().setBatchSize(maxBatch);
    /** Extract model name and load its weights **/
    std::string binFileName = fileNameNoExt(pathToModel) + ".bin";
    netReader.ReadWeights(binFileName);
    /** Read labels (if any)**/
    std::string labelFileName = fileNameNoExt(pathToModel) + ".labels";

    std::ifstream inputFile(labelFileName);
    std::copy(std::istream_iterator<std::string>(inputFile),
              std::istream_iterator<std::string>(),
              std::back_inserter(labels));
    // -----------------------------------------------------------------------------------------------------

    /** SSD-based network should have one input and one output **/
    // ---------------------------Check inputs -------------------------------------------------------------
    slog::info << "Checking Face Detection network inputs" << slog::endl;
    InputsDataMap inputInfo(netReader.getNetwork().getInputsInfo());
    if (inputInfo.size() != 1) {
        throw std::logic_error("Face Detection network should have only one input");
    }
    InputInfo::Ptr inputInfoFirst = inputInfo.begin()->second;
    inputInfoFirst->setPrecision(Precision::U8);
    // -----------------------------------------------------------------------------------------------------

    // ---------------------------Check outputs ------------------------------------------------------------
    slog::info << "Checking Face Detection network outputs" << slog::endl;
    OutputsDataMap outputInfo(netReader.getNetwork().getOutputsInfo());
    if (outputInfo.size() != 1) {
        throw std::logic_error("Face Detection network should have only one output");
    }
    DataPtr& _output = outputInfo.begin()->second;
    output = outputInfo.begin()->first;

    const CNNLayerPtr outputLayer = netReader.getNetwork().getLayerByName(output.c_str());
    if (outputLayer->type != "DetectionOutput") {
        throw std::logic_error("Face Detection network output layer(" + outputLayer->name +
                               ") should be DetectionOutput, but was " +  outputLayer->type);
    }

    if (outputLayer->params.find("num_classes") == outputLayer->params.end()) {
        throw std::logic_error("Face Detection network output layer (" +
                               output + ") should have num_classes integer attribute");
    }

    const int num_classes = outputLayer->GetParamAsInt("num_classes");
    if (labels.size() != num_classes) {
        if (labels.size() == (num_classes - 1))  // if network assumes default "background" class, which has no label
            labels.insert(labels.begin(), "fake");
        else
            labels.clear();
    }
    const SizeVector outputDims = _output->getTensorDesc().getDims();
    maxProposalCount = outputDims[2];
    objectSize = outputDims[3];
    if (objectSize != 7) {
        throw std::logic_error("Face Detection network output layer should have 7 as a last dimension");
    }
    if (outputDims.size() != 4) {
        throw std::logic_error("Face Detection network output dimensions not compatible shoulld be 4, but was " +
                               std::to_string(outputDims.size()));
    }
    _output->setPrecision(Precision::FP32);

    slog::info << "Loading Face Detection model to the "<< deviceForInference << " plugin" << slog::endl;
    input = inputInfo.begin()->first;
    return netReader.getNetwork();
}

void FaceDetection::fetchResults() {
    if (!enabled()) return;
    results.clear();
    if (resultsFetched) return;
    resultsFetched = true;
    const float *detections = request->GetBlob(output)->buffer().as<float *>();

    for (int i = 0; i < maxProposalCount; i++) {
        float image_id = detections[i * objectSize + 0];
        Result r;
        r.label = static_cast<int>(detections[i * objectSize + 1]);
        r.confidence = detections[i * objectSize + 2];

        if (r.confidence <= detectionThreshold) {
            continue;
        }

        r.location.x = detections[i * objectSize + 3] * width;
        r.location.y = detections[i * objectSize + 4] * height;
        r.location.width = detections[i * objectSize + 5] * width - r.location.x;
        r.location.height = detections[i * objectSize + 6] * height - r.location.y;

        // Make square and enlarge face bounding box for more robust operation of face analytics networks
        int bb_width = r.location.width;
        int bb_height = r.location.height;

        int bb_center_x = r.location.x + bb_width / 2;
        int bb_center_y = r.location.y + bb_height / 2;

        int max_of_sizes = std::max(bb_width, bb_height);

        int bb_new_width = bb_enlarge_coefficient * max_of_sizes;
        int bb_new_height = bb_enlarge_coefficient * max_of_sizes;

        r.location.x = bb_center_x - bb_new_width / 2;
        r.location.y = bb_center_y - bb_new_height / 2;

        r.location.width = bb_new_width;
        r.location.height = bb_new_height;

        if (image_id < 0) {
            break;
        }
        if (doRawOutputMessages) {
            std::cout << "[" << i << "," << r.label << "] element, prob = " << r.confidence <<
                         "    (" << r.location.x << "," << r.location.y << ")-(" << r.location.width << ","
                      << r.location.height << ")"
                      << ((r.confidence > detectionThreshold) ? " WILL BE RENDERED!" : "") << std::endl;
        }

        results.push_back(r);
    }
}

FacialLandmarksDetection::FacialLandmarksDetection(const std::string &pathToModel,
                                                   const std::string &deviceForInference,
                                                   int maxBatch, bool isBatchDynamic, bool isAsync)
    : BaseDetection("Facial Landmarks", pathToModel, deviceForInference, maxBatch, isBatchDynamic, isAsync),
      outputFacialLandmarksBlobName("align_fc3"), enquedFaces(0) {
}

void FacialLandmarksDetection::submitRequest() {
    if (!enquedFaces) return;
    if (isBatchDynamic) {
        request->SetBatch(enquedFaces);
    }
    BaseDetection::submitRequest();
    enquedFaces = 0;
}

void FacialLandmarksDetection::enqueue(const cv::Mat &face) {
    if (!enabled()) {
        return;
    }
    if (enquedFaces == maxBatch) {
        slog::warn << "Number of detected faces more than maximum(" << maxBatch << ") processed by Facial Landmarks estimator" << slog::endl;
        return;
    }
    if (!request) {
        request = net.CreateInferRequestPtr();
    }

    Blob::Ptr inputBlob = request->GetBlob(input);

    matU8ToBlob<uint8_t>(face, inputBlob, enquedFaces);

    enquedFaces++;
}

std::vector<float> FacialLandmarksDetection::operator[] (int idx) const {
    std::vector<float> normedLandmarks;

    auto landmarksBlob = request->GetBlob(outputFacialLandmarksBlobName);
    auto n_lm = landmarksBlob->dims()[0];
    const float *normed_coordinates = request->GetBlob(outputFacialLandmarksBlobName)->buffer().as<float *>();

    for (auto i = 0UL; i < n_lm; ++i)
        normedLandmarks.push_back(normed_coordinates[i + n_lm * idx]);

    return normedLandmarks;
}

CNNNetwork FacialLandmarksDetection::read() {
    slog::info << "Loading network files for Facial Landmarks Estimation" << slog::endl;
    CNNNetReader netReader;
    // Read network model
    netReader.ReadNetwork(pathToModel);
    // Set maximum batch size
    netReader.getNetwork().setBatchSize(maxBatch);
    slog::info << "Batch size is set to  " << netReader.getNetwork().getBatchSize() << " for Facial Landmarks Estimation network" << slog::endl;
    // Extract model name and load its weights
    std::string binFileName = fileNameNoExt(pathToModel) + ".bin";
    netReader.ReadWeights(binFileName);

    // ---------------------------Check inputs -------------------------------------------------------------
    slog::info << "Checking Facial Landmarks Estimation network inputs" << slog::endl;
    InputsDataMap inputInfo(netReader.getNetwork().getInputsInfo());
    if (inputInfo.size() != 1) {
        throw std::logic_error("Facial Landmarks Estimation network should have only one input");
    }
    InputInfo::Ptr& inputInfoFirst = inputInfo.begin()->second;
    inputInfoFirst->setPrecision(Precision::U8);
    input = inputInfo.begin()->first;
    // -----------------------------------------------------------------------------------------------------

    // ---------------------------Check outputs ------------------------------------------------------------
    slog::info << "Checking Facial Landmarks Estimation network outputs" << slog::endl;
    OutputsDataMap outputInfo(netReader.getNetwork().getOutputsInfo());
    if (outputInfo.size() != 1) {
        throw std::logic_error("Facial Landmarks Estimation network should have only one output");
    }
    for (auto& output : outputInfo) {
        output.second->setPrecision(Precision::FP32);
    }
    std::map<std::string, bool> layerNames = {
        {outputFacialLandmarksBlobName, false}
    };

    for (auto && output : outputInfo) {
        CNNLayerPtr layer = output.second->getCreatorLayer().lock();
        if (!layer) {
            throw std::logic_error("Layer pointer is invalid");
        }
        if (layerNames.find(layer->name) == layerNames.end()) {
            throw std::logic_error("Facial Landmarks Estimation network output layer unknown: " + layer->name + ", should be " +
                                   outputFacialLandmarksBlobName);
        }
        if (layer->type != "FullyConnected") {
            throw std::logic_error("Facial Landmarks Estimation network output layer (" + layer->name + ") has invalid type: " +
                                   layer->type + ", should be FullyConnected");
        }
        auto fc = dynamic_cast<FullyConnectedLayer*>(layer.get());
        if (!fc) {
            throw std::logic_error("Fully connected layer is not valid");
        }
        if (fc->_out_num != 70) {
            throw std::logic_error("Facial Landmarks Estimation network output layer (" + layer->name + ") has invalid out-size=" +
                                   std::to_string(fc->_out_num) + ", should be 70");
        }
        layerNames[layer->name] = true;
    }

    slog::info << "Loading Facial Landmarks Estimation model to the "<< deviceForInference << " plugin" << slog::endl;

    _enabled = true;
    return netReader.getNetwork();
}


Load::Load(BaseDetection& detector) : detector(detector) {
}

void Load::into(InferencePlugin & plg, bool enable_dynamic_batch) const {
    if (detector.enabled()) {
        std::map<std::string, std::string> config;
        if (enable_dynamic_batch) {
            config[PluginConfigParams::KEY_DYN_BATCH_ENABLED] = PluginConfigParams::YES;
        }
        detector.net = plg.LoadNetwork(detector.read(), config);
        detector.plugin = &plg;
    }
}


CallStat::CallStat():
    _number_of_calls(0), _total_duration(0.0), _last_call_duration(0.0), _smoothed_duration(-1.0) {
}

double CallStat::getSmoothedDuration() {
    // Additional check is needed for the first frame while duration of the first
    // visualisation is not calculated yet.
    if (_smoothed_duration < 0) {
        auto t = std::chrono::high_resolution_clock::now();
        return std::chrono::duration_cast<ms>(t - _last_call_start).count();
    }
    return _smoothed_duration;
}

double CallStat::getTotalDuration() {
    return _total_duration;
}

void CallStat::calculateDuration() {
    auto t = std::chrono::high_resolution_clock::now();
    _last_call_duration = std::chrono::duration_cast<ms>(t - _last_call_start).count();
    _number_of_calls++;
    _total_duration += _last_call_duration;
    if (_smoothed_duration < 0) {
        _smoothed_duration = _last_call_duration;
    }
    double alpha = 0.1;
    _smoothed_duration = _smoothed_duration * (1.0 - alpha) + _last_call_duration * alpha;
}

void CallStat::setStartTime() {
    _last_call_start = std::chrono::high_resolution_clock::now();
}


void Timer::start(const std::string& name) {
    if (_timers.find(name) == _timers.end()) {
        _timers[name] = CallStat();
    }
    _timers[name].setStartTime();
}

void Timer::finish(const std::string& name) {
    auto& timer = (*this)[name];
    timer.calculateDuration();
}

CallStat& Timer::operator[](const std::string& name) {
    if (_timers.find(name) == _timers.end()) {
        throw std::logic_error("No timer with name " + name + ".");
    }
    return _timers[name];
}
