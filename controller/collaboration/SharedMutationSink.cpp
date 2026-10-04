#include "collaboration/SharedMutationSink.hpp"
#include "collaboration/CollaborationState.hpp"

namespace daw::collab {
void prepareFixedColorEdits(CommandBody& body, const SharedProjectDocument& document) {
    if (auto* batch=std::get_if<std::shared_ptr<BatchCommand>>(&body)) {
        if (!*batch) return;
        std::vector<ProjectCommand> prepared;
        for (auto& child:(*batch)->commands) {
            const bool add=std::holds_alternative<AddPluginInsert>(child.body);
            prepareFixedColorEdits(child.body,document);
            if (add && std::holds_alternative<std::shared_ptr<BatchCommand>>(child.body)) {
                auto& restored=*std::get<std::shared_ptr<BatchCommand>>(child.body);
                restored.commands.front().conditions=std::move(child.conditions);
                for(auto& command:restored.commands) prepared.push_back(std::move(command));
            } else prepared.push_back(std::move(child));
        }
        (*batch)->commands=std::move(prepared);
        return;
    }
    const auto* add=std::get_if<AddPluginInsert>(&body);
    if (!add || add->location.chain!=PluginChain::ChannelColor) return;
    const auto found=document.deletedPluginInserts.find(add->insert.id);
    if (found==document.deletedPluginInserts.end()) return;
    const auto& deleted=found->second;
    if (deleted.location.chain!=add->location.chain ||
        deleted.location.trackId!=add->location.trackId ||
        deleted.location.clipId!=add->location.clipId) return;
    auto batch=std::make_shared<BatchCommand>();
    ProjectCommand restore,replace;
    restore.body=RestorePluginInsert{add->location,add->insert.id,deleted.deleteOperationId};
    replace.body=ReplacePluginInsert{add->location,add->insert.id,add->insert};
    batch->commands.push_back(std::move(restore)); batch->commands.push_back(std::move(replace));
    body=std::move(batch);
}
} // namespace daw::collab
