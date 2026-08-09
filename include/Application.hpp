#pragma once

#include <filesystem>
#include <memory>

namespace maskui {

class Application {
public:
    explicit Application(
        std::filesystem::path executablePath,
        std::filesystem::path initialModelPath = {});
    ~Application();

    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    int run();

private:
    class Implementation;
    std::unique_ptr<Implementation> implementation_;
};

} // namespace maskui
