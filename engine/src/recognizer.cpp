#include "swipekb/recognizer.hpp"

namespace swipekb {
struct Recognizer::Impl {};
Recognizer::Recognizer() : impl_(std::make_unique<Impl>()) {}
Recognizer::~Recognizer() = default;
bool Recognizer::init(const ModelPaths &) { return false; }
bool Recognizer::ready() const { return false; }
std::vector<Candidate> Recognizer::recognize(const std::vector<TracePoint> &, const std::vector<KeyRect> &,
                                             const std::vector<std::string> &, int) { return {}; }
} // namespace swipekb
