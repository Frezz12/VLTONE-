#include "EngineController.hpp"
#include "model/ChannelColor.hpp"
#include <algorithm>
#include <cmath>

namespace daw {
InsertModel EngineController::channelColorSettings(const std::string& id) const {
    const auto* track=m_project.findTrack(id);
    return track && track->channelColor ? *track->channelColor : defaultChannelColor(id);
}

void EngineController::applyChannelColorState(const std::string& id, const std::optional<InsertModel>& state) {
    auto* track=m_project.findTrack(id);
    if (!track || !supportsChannelColor(track->kind)) return;
    unfreezeTrack(id,false);
    track=m_project.findTrack(id);
    track->channelColor=state;
    const auto settings=channelColorSettings(id);
    const auto apply = [&] { return m_runtime.configureChannelColor({id, settings.id},
        parseChannelColorSeed(settings.profileSeed), settings.parameters, settings.bypassed); };
    if (!apply()) { rebuildGraph(); (void)apply(); }
}

void EngineController::setChannelColorParameter(const std::string& id, const std::string& param, double value) {
    auto* track=m_project.findTrack(id);
    if (!track || !supportsChannelColor(track->kind) || !std::isfinite(value) ||
        (param!="drive" && param!="tone") || !sharedEditingAllowed()) return;
    const auto settings=channelColorSettings(id);
    if (!sharedGestureAllowed("plugin:"+settings.id)) return;
    unfreezeTrack(id,false);
    track=m_project.findTrack(id);
    if (!track->channelColor) track->channelColor=settings;
    setInsertParameter(id,settings.id,param,std::clamp(value,-100.0,100.0));
}

void EngineController::setChannelColorEnabled(const std::string& id, bool enabled) {
    const auto* track=m_project.findTrack(id);
    if (!track || !supportsChannelColor(track->kind) || !sharedEditingAllowed()) return;
    const auto before=track->channelColor;
    auto settings=channelColorSettings(id);
    if (settings.bypassed==!enabled || !sharedGestureAllowed("plugin:"+settings.id)) return;
    settings.bypassed=!enabled;
    applyChannelColorState(id,settings);
    commitChannelColorEdit(id,before,enabled?"Enable COLOR":"Bypass COLOR");
}

void EngineController::commitChannelColorEdit(const std::string& id, const std::optional<InsertModel>& before,
                                             const std::string& label) {
    const auto* track=m_project.findTrack(id);
    if (!track || !track->channelColor) return;
    const auto after=track->channelColor;
    const auto baseline=before.value_or(defaultChannelColor(id));
    if (baseline.parameters==after->parameters && baseline.bypassed==after->bypassed) {
        if (!before) applyChannelColorState(id,before);
        return;
    }
    if (!sharedGestureAllowed("plugin:"+after->id)) {
        restoreSharedPreview([&]{ applyChannelColorState(id,before); }); return;
    }
    const collab::PluginLocation location{collab::PluginChain::ChannelColor,id,{}};
    collab::CommandBody body;
    if (!before) {
        auto clean=*after; clean.path.clear(); clean.stateFile.clear(); clean.rightStateFile.clear();
        body=collab::AddPluginInsert{location,std::move(clean),{}};
    } else {
        auto batch=std::make_shared<collab::BatchCommand>();
        const auto append=[&](collab::CommandBody command) {
            collab::ProjectCommand child; child.body=std::move(command); batch->commands.push_back(std::move(child));
        };
        if (before->bypassed!=after->bypassed)
            append(collab::SetPluginProperty{location,after->id,collab::PluginProperty::Bypassed,after->bypassed});
        for (const auto& param:after->parameters) {
            const auto found=std::find_if(before->parameters.begin(),before->parameters.end(),
                                         [&](const auto& old){return old.id==param.id;});
            if (found==before->parameters.end() || found->value!=param.value)
                append(collab::SetPluginParameter{location,after->id,param.id,param.value,false});
        }
        body=std::move(batch);
    }
    const auto shared=submitSharedMutation(std::move(body),label);
    if (shared!=collab::SharedMutationResult::LocalFallback) {
        if (shared==collab::SharedMutationResult::Blocked)
            restoreSharedPreview([&]{ applyChannelColorState(id,before); });
        return;
    }
    m_undo.push(label,[this,id,before]{applyChannelColorState(id,before);},
                      [this,id,after]{applyChannelColorState(id,after);});
}
} // namespace daw
