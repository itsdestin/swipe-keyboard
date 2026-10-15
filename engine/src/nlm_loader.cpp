// Opens the neural-model library on demand (see nlm.hpp for why it is not linked directly).
#include "swipekb/nlm.hpp"

#include <dlfcn.h>

#include <cstdio>
#include <fstream>
#include <string>

namespace swipekb {

namespace {

// The folder this engine library sits in. WHY: libswipekb_nlm.so is installed next to it (like the Qt plugin
// finds the engine), and an absolute path never depends on the program's library search settings.
std::string ownDir()
{
    Dl_info info{};
    if (!dladdr(reinterpret_cast<void *>(&ownDir), &info) || !info.dli_fname)
        return {};
    const std::string p = info.dli_fname;
    const auto slash = p.rfind('/');
    return slash == std::string::npos ? std::string() : p.substr(0, slash);
}

} // namespace

std::unique_ptr<WordScorer> loadNeuralLM(const std::string &modelPath, const NeuralLMOptions &options)
{
    if (modelPath.empty() || !std::ifstream(modelPath).good()) {
        std::fprintf(stderr, "swipekb: neural model %s not found; ranking without it\n", modelPath.c_str());
        return nullptr;
    }
    const std::string dir = ownDir();
    const std::string lib = (dir.empty() ? std::string() : dir + "/") + "libswipekb_nlm.so";
    // WHY never closed: the scorer's code lives in that library, and the keyboard keeps it for its whole life.
    void *h = dlopen(lib.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        std::fprintf(stderr, "swipekb: cannot open %s (%s); ranking without the neural model\n", lib.c_str(), dlerror());
        return nullptr;
    }
    using Create = WordScorer *(*)(const char *, const NeuralLMOptions *);
    auto create = reinterpret_cast<Create>(dlsym(h, "swipekb_nlm_create"));
    if (!create) {
        std::fprintf(stderr, "swipekb: %s has no swipekb_nlm_create; ranking without the neural model\n", lib.c_str());
        return nullptr;
    }
    std::unique_ptr<WordScorer> s(create(modelPath.c_str(), &options));
    if (!s)
        std::fprintf(stderr, "swipekb: could not load neural model %s; ranking without it\n", modelPath.c_str());
    return s;
}

} // namespace swipekb
