#ifndef MAGCAL_H
#define MAGCAL_H

#include <Arduino.h>

struct MagCalResult {
    float minX;
    float maxX;
    float minY;
    float maxY;
    float minZ;
    float maxZ;
    float offX;
    float offY;
    float offZ;
    float rngX;
    float rngY;
    float rngZ;
    float confidence;
    uint32_t sampleCount;
    uint32_t elapsedMs;
    bool stoppedByUser;
};

class MagCal {
public:
    void start();
    void update(float magX, float magY, float magZ);
    void handleSerial(Stream &serial);
    void stopByUser();
    bool isDone() const;
    MagCalResult getResult() const;

private:
    void recompute();

    bool started_ = false;
    bool done_ = false;
    bool stoppedByUser_ = false;
    uint32_t startMs_ = 0;
    uint32_t sampleCount_ = 0;

    float minX_ = 0.0f;
    float maxX_ = 0.0f;
    float minY_ = 0.0f;
    float maxY_ = 0.0f;
    float minZ_ = 0.0f;
    float maxZ_ = 0.0f;
    float offX_ = 0.0f;
    float offY_ = 0.0f;
    float offZ_ = 0.0f;
    float rngX_ = 0.0f;
    float rngY_ = 0.0f;
    float rngZ_ = 0.0f;
    float confidence_ = 0.0f;
};

#endif
