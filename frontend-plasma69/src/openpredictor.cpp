// SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// FUTO-free swipekb::Predictor for the Plasma 6.9 front end (interim, see openengine.h).
//
// WHY its own definition of the shared class: plugin/nextwordworker.cpp (reused unchanged) runs swipekb::Predictor on
// its thread, but engine/src/prediction.cpp also carries the English ExecuTorch path. This file gives the same class
// with only the open n-gram + LANG.nwp.bin path, so this front end never links FUTO's library. The drop-FUTO branch
// makes engine/src/prediction.cpp itself FUTO-free; this file is deleted then.

#include "swipekb/nwp_predictor.hpp"
#include "swipekb/prediction.hpp"

#include <algorithm>
#include <fstream>

namespace swipekb
{
struct Predictor::Impl {
    NwpPredictor ngram;
};

Predictor::Predictor() = default;
Predictor::~Predictor() = default;

bool Predictor::ready() const
{
    return m_impl && m_impl->ngram.ready();
}

bool Predictor::init(const std::string &model, const std::string &vocab, const std::string &nwp, bool siblingFallback)
{
    m_impl.reset();
    if (model.empty() || vocab.empty()) {
        return false;
    }
    try {
        std::ifstream probe(model, std::ios::binary);
        char magic[8]{};
        probe.read(magic, sizeof magic);
        // WHY refuse anything but our n-gram format: a FUTO .pte context model must never load here.
        if (probe.gcount() != sizeof magic || std::string(magic, sizeof magic) != std::string("SWKBNG2\0", 8)) {
            return false;
        }
        auto impl = std::make_unique<Impl>();
        if (!impl->ngram.load(model, vocab, nwp, siblingFallback)) {
            return false;
        }
        m_impl = std::move(impl);
        return true;
    } catch (...) {
        return false;
    }
}

std::vector<std::string> Predictor::predict(const std::vector<std::string> &context, int topK, const std::string &prefix)
{
    if (!ready() || topK <= 0) {
        return {};
    }
    try {
        return m_impl->ngram.predict(context, std::min(topK, 64), prefix);
    } catch (...) {
        return {};
    }
}
} // namespace swipekb
