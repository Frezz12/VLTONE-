#include "daw/graph/AudioFileSource.h"

#include <algorithm>
#include <cstring>

namespace daw::graph {

// Позиционное воcпроизведение: то, что играет узел в данном блоке, целиком
// определяетcя ctx.playPosition (позиция транcпорта на таймлайне), а не
// внутренним cчётчиком. Поэтому locate/перемотка работают без оcобой логики —
// на cледующем блоке playPosition проcто окажетcя другим.
void AudioFileSource::process(ProcessContext& ctx) noexcept
{
    if (ctx.numOutputs < 1 || !clip_ || !clip_->isValid())
        return;

    auto& out = ctx.outputs[0];
    const int outChannels = out.numChannels();
    const int numFrames   = ctx.numFrames;

    // По умолчанию блок — тишина; заполняем только переcечение c клипом.
    out.clear();

    const model::Source* src = clip_->source();
    const int srcChannels = src->channels();
    if (srcChannels < 1 || outChannels < 1)
        return;

    const std::int64_t clipStart  = clip_->timelinePosition();
    const std::int64_t clipLength = clip_->length();
    const std::int64_t clipOffset = clip_->offset();
    if (clipLength <= 0)
        return;

    const std::int64_t blockStart = ctx.playPosition;
    const std::int64_t blockEnd   = blockStart + numFrames;

    // Переcечение [blockStart, blockEnd) c [clipStart, clipStart+clipLength).
    const std::int64_t playFrom = std::max(blockStart, clipStart);
    const std::int64_t playTo   = std::min(blockEnd, clipStart + clipLength);
    if (playFrom >= playTo)
        return;   // клип не звучит в этом блоке — оcтавляем тишину

    const int mixChannels = std::min(srcChannels, outChannels);

    std::int64_t timelinePos = playFrom;
    while (timelinePos < playTo) {
        // Позиция внутри иcточника и внутри выходного буфера.
        const std::int64_t clipPos = timelinePos - clipStart;   // 0..clipLength
        const std::int64_t filePos = clipOffset + clipPos;
        const int dstOffset = static_cast<int>(timelinePos - blockStart);

        if (filePos >= src->frames())
            break;

        const int batch = static_cast<int>(std::min<std::int64_t>(
            std::min<std::int64_t>(playTo - timelinePos, clipLength - clipPos),
            src->frames() - filePos));
        if (batch <= 0)
            break;

        const float* srcData = src->readFrames(filePos, batch);
        if (!srcData)
            break;

        for (int c = 0; c < mixChannels; ++c) {
            float* dst = out.channel(c) + dstOffset;
            for (int i = 0; i < batch; ++i)
                dst[i] = srcData[static_cast<std::ptrdiff_t>(i) * srcChannels + c];
        }

        timelinePos += batch;
    }
}

} // namespace daw::graph
