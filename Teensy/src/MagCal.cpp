#include "MagCal.h"

#include <algorithm>
#include <float.h>

namespace {
constexpr float kAutoCompleteConfidence = 0.90f;
constexpr uint32_t kMinAutoCompleteSamples = 1500U;
constexpr uint32_t kMinAutoCompleteMs = 20000U;
}

void MagCal::start() {
    started_ = true;
    done_ = false;
    stoppedByUser_ = false;
    startMs_ = millis();
    sampleCount_ = 0;

    minX_ = FLT_MAX;
    minY_ = FLT_MAX;
    minZ_ = FLT_MAX;
    maxX_ = -FLT_MAX;
    maxY_ = -FLT_MAX;
    maxZ_ = -FLT_MAX;
    offX_ = 0.0f;
    offY_ = 0.0f;
    offZ_ = 0.0f;
    rngX_ = 0.0f;
    rngY_ = 0.0f;
    rngZ_ = 0.0f;
    confidence_ = 0.0f;
}

void MagCal::update(float magX, float magY, float magZ) {
    if (!started_ || done_) {
        return;
    }

    minX_ = std::min(minX_, magX);
    minY_ = std::min(minY_, magY);
    minZ_ = std::min(minZ_, magZ);
    maxX_ = std::max(maxX_, magX);
    maxY_ = std::max(maxY_, magY);
    maxZ_ = std::max(maxZ_, magZ);
    sampleCount_++;

    recompute();
    const uint32_t elapsedMs = millis() - startMs_;
    if ((sampleCount_ >= kMinAutoCompleteSamples) &&
        (elapsedMs >= kMinAutoCompleteMs) &&
        (confidence_ >= kAutoCompleteConfidence)) {
        done_ = true;
    }
}

void MagCal::handleSerial(Stream &serial) {
    if (!started_ || done_) {
        return;
    }
    while (serial.available() > 0) {
        const char c = (char)serial.read();
        if (c == 'x' || c == 'X') {
            stopByUser();
        }
    }
}

void MagCal::stopByUser() {
    stoppedByUser_ = true;
    done_ = true;
}

bool MagCal::isDone() const {
    return done_;
}

MagCalResult MagCal::getResult() const {
    MagCalResult out{};
    if (sampleCount_ == 0) {
        out.minX = 0.0f;
        out.maxX = 0.0f;
        out.minY = 0.0f;
        out.maxY = 0.0f;
        out.minZ = 0.0f;
        out.maxZ = 0.0f;
    } else {
        out.minX = minX_;
        out.maxX = maxX_;
        out.minY = minY_;
        out.maxY = maxY_;
        out.minZ = minZ_;
        out.maxZ = maxZ_;
    }
    out.offX = offX_;
    out.offY = offY_;
    out.offZ = offZ_;
    out.rngX = rngX_;
    out.rngY = rngY_;
    out.rngZ = rngZ_;
    out.confidence = confidence_;
    out.sampleCount = sampleCount_;
    out.elapsedMs = millis() - startMs_;
    out.stoppedByUser = stoppedByUser_;
    return out;
}

void MagCal::recompute() {
    offX_ = (maxX_ + minX_) * 0.5f;
    offY_ = (maxY_ + minY_) * 0.5f;
    offZ_ = (maxZ_ + minZ_) * 0.5f;
    rngX_ = maxX_ - minX_;
    rngY_ = maxY_ - minY_;
    rngZ_ = maxZ_ - minZ_;

    const float maxRange = std::max(rngX_, std::max(rngY_, rngZ_));
    const float minRange = std::min(rngX_, std::min(rngY_, rngZ_));
    if (maxRange <= 1.0e-6f) {
        confidence_ = 0.0f;
        return;
    }
    float c = minRange / maxRange;
    if (c < 0.0f) c = 0.0f;
    if (c > 1.0f) c = 1.0f;
    confidence_ = c;
}
