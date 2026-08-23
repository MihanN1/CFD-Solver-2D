#pragma once
#include <string>


namespace progress {

void begin(const std::string& title, double startAt, double total,
           const std::string& outputDir);

void update(double current);

void finish(bool ok);

bool stopRequested();

void requestStop();

void shutdown();

}   // namespace progress
