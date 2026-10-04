#pragma once

#include "Host/PluginTypes.hpp"

/// The host-side COM objects VST3 requires a host to provide.
///
/// VST3 is the only one of the three formats where the host has to *implement*
/// interfaces rather than just call them, so this is boilerplate no amount of
/// design avoids. It lives in its own header to keep `Vst3Instance` about the
/// plugin rather than about `queryInterface`.
///
/// Everything the host owns as a member uses `ImplementsNonDestroyable`: a
/// plugin that over-releases a host object would otherwise free memory the host
/// still owns, and that failure mode is untraceable.

#include <pluginterfaces/base/funknownimpl.h>
#include <pluginterfaces/base/ibstream.h>
#include <pluginterfaces/gui/iplugview.h>
#include <pluginterfaces/vst/ivstattributes.h>
#include <pluginterfaces/vst/ivsteditcontroller.h>
#include <pluginterfaces/vst/ivstevents.h>
#include <pluginterfaces/vst/ivsthostapplication.h>
#include <pluginterfaces/vst/ivstmessage.h>
#include <pluginterfaces/vst/ivstparameterchanges.h>
#include <pluginterfaces/vst/ivstpluginterfacesupport.h>
#include <pluginterfaces/vst/ivstmidicontrollers.h>

#include <algorithm>
#include <bit>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <string>
#include <vector>

namespace daw::plugins::vst3 {

using namespace Steinberg;

/// An `IBStream` over a byte vector — how plugin state gets in and out.
class MemoryStream final : public U::Implements<U::Directly<IBStream>> {
public:
    MemoryStream() = default;
    explicit MemoryStream(std::vector<std::uint8_t> data) : m_data(std::move(data)) {}

    const std::vector<std::uint8_t>& data() const noexcept { return m_data; }
    bool failed() const noexcept { return m_failed; }
    void rewind() noexcept { m_position = 0; }

    tresult PLUGIN_API read(void* buffer, int32 numBytes, int32* numBytesRead) override {
        if (numBytesRead) *numBytesRead = 0;
        if (numBytes < 0 || (numBytes > 0 && !buffer)) return kInvalidArgument;
        if (m_position >= m_data.size()) {
            if (numBytesRead) *numBytesRead = 0;
            return kResultOk;
        }
        const std::size_t remaining = m_data.size() - m_position;
        const std::size_t count = std::min<std::size_t>(remaining, std::size_t(numBytes));
        if (count > 0) std::memcpy(buffer, m_data.data() + m_position, count);
        m_position += count;
        if (numBytesRead) *numBytesRead = int32(count);
        return kResultOk;
    }

    tresult PLUGIN_API write(void* buffer, int32 numBytes, int32* numBytesWritten) override {
        if (numBytesWritten) *numBytesWritten = 0;
        const auto reject = [&](tresult error) { m_failed = true; return error; };
        if (numBytes < 0 || (numBytes > 0 && !buffer)) return reject(kInvalidArgument);
        const std::size_t count = std::size_t(numBytes);
        if (!count) return kResultOk;
        if (m_position > kMaxPluginStateBytes || count > kMaxPluginStateBytes - m_position) {
            return reject(kOutOfMemory);
        }
        const auto* bytes = static_cast<const std::uint8_t*>(buffer);
        if (m_position + count > m_data.size()) {
            try { m_data.resize(m_position + count); }
            catch (...) { return reject(kOutOfMemory); }
        }
        if (count > 0) std::memcpy(m_data.data() + m_position, bytes, count);
        m_position += count;
        if (numBytesWritten) *numBytesWritten = numBytes;
        return kResultOk;
    }

    tresult PLUGIN_API seek(int64 pos, int32 mode, int64* result) override {
        int64 base = 0;
        switch (mode) {
            case kIBSeekSet: break;
            case kIBSeekCur: base = int64(m_position); break;
            case kIBSeekEnd: base = int64(m_data.size()); break;
            default: return kInvalidArgument;
        }
        if (base < 0 || base > int64(kMaxPluginStateBytes) ||
            pos < -base || pos > int64(kMaxPluginStateBytes) - base) return kInvalidArgument;
        const int64 target = base + pos;
        // Seeking past the end is legal and does not grow the buffer; a write
        // there is what grows it.
        m_position = std::size_t(target);
        if (result) *result = target;
        return kResultOk;
    }

    tresult PLUGIN_API tell(int64* pos) override {
        if (!pos) return kInvalidArgument;
        *pos = int64(m_position);
        return kResultOk;
    }

private:
    std::vector<std::uint8_t> m_data;
    std::size_t m_position = 0;
    bool m_failed = false;
};

/// The bag of typed values a plugin passes between its processor and its
/// controller. Both halves live in our process, so this only has to be
/// self-consistent, not interoperable with anyone else's.
class HostAttributeList final : public U::Implements<U::Directly<Vst::IAttributeList>> {
public:
    tresult PLUGIN_API setInt(AttrID id, int64 value) override {
        m_values[id].integer = value;
        m_values[id].kind = Value::Kind::Integer;
        return kResultOk;
    }
    tresult PLUGIN_API getInt(AttrID id, int64& value) override {
        auto it = m_values.find(id);
        if (it == m_values.end() || it->second.kind != Value::Kind::Integer) {
            return kResultFalse;
        }
        value = it->second.integer;
        return kResultOk;
    }
    tresult PLUGIN_API setFloat(AttrID id, double value) override {
        m_values[id].real = value;
        m_values[id].kind = Value::Kind::Real;
        return kResultOk;
    }
    tresult PLUGIN_API getFloat(AttrID id, double& value) override {
        auto it = m_values.find(id);
        if (it == m_values.end() || it->second.kind != Value::Kind::Real) return kResultFalse;
        value = it->second.real;
        return kResultOk;
    }
    tresult PLUGIN_API setString(AttrID id, const Vst::TChar* string) override {
        Value& value = m_values[id];
        value.kind = Value::Kind::String;
        value.text.clear();
        for (const Vst::TChar* c = string; c && *c; ++c) value.text.push_back(*c);
        value.text.push_back(0);
        return kResultOk;
    }
    tresult PLUGIN_API getString(AttrID id, Vst::TChar* string, uint32 sizeInBytes) override {
        auto it = m_values.find(id);
        if (it == m_values.end() || it->second.kind != Value::Kind::String) return kResultFalse;
        const std::size_t characters =
            std::min<std::size_t>(it->second.text.size(), sizeInBytes / sizeof(Vst::TChar));
        if (characters == 0) return kResultFalse;
        std::memcpy(string, it->second.text.data(), characters * sizeof(Vst::TChar));
        string[characters - 1] = 0;
        return kResultOk;
    }
    tresult PLUGIN_API setBinary(AttrID id, const void* data, uint32 sizeInBytes) override {
        Value& value = m_values[id];
        value.kind = Value::Kind::Binary;
        const auto* bytes = static_cast<const std::uint8_t*>(data);
        value.binary.assign(bytes, bytes + sizeInBytes);
        return kResultOk;
    }
    tresult PLUGIN_API getBinary(AttrID id, const void*& data, uint32& sizeInBytes) override {
        auto it = m_values.find(id);
        if (it == m_values.end() || it->second.kind != Value::Kind::Binary) return kResultFalse;
        data = it->second.binary.data();
        sizeInBytes = uint32(it->second.binary.size());
        return kResultOk;
    }

private:
    struct Value {
        enum class Kind { Integer, Real, String, Binary } kind = Kind::Integer;
        int64 integer = 0;
        double real = 0.0;
        std::vector<Vst::TChar> text;
        std::vector<std::uint8_t> binary;
    };
    std::map<std::string, Value> m_values;
};

/// One message between a plugin's two halves.
class HostMessage final : public U::Implements<U::Directly<Vst::IMessage>> {
public:
    HostMessage() { m_attributes = owned(new HostAttributeList); }

    const char* PLUGIN_API getMessageID() override { return m_id.c_str(); }
    void PLUGIN_API setMessageID(const char* id) override { m_id = id ? id : ""; }
    Vst::IAttributeList* PLUGIN_API getAttributes() override { return m_attributes; }

private:
    std::string m_id;
    IPtr<HostAttributeList> m_attributes;
};

/// What a plugin queries the host for during `initialize`. Plugins that cannot
/// get an `IMessage` from here fall back to not talking between their halves,
/// which shows up as an editor that never updates.
class HostApplication final
    : public U::ImplementsNonDestroyable<
          U::Directly<Vst::IHostApplication, Vst::IPlugInterfaceSupport>> {
public:
    tresult PLUGIN_API getName(Vst::String128 name) override {
        static const char16_t kName[] = u"VLTONE";
        std::memcpy(name, kName, sizeof(kName));
        return kResultOk;
    }

    tresult PLUGIN_API createInstance(TUID cid, TUID iid, void** obj) override {
        if (!obj) return kInvalidArgument;
        const FUID classId = FUID::fromTUID(cid);
        const FUID interfaceId = FUID::fromTUID(iid);
        // The cast to the interface is **not** cosmetic. `U::Implements` lists
        // its own base class before the interface, so the most-derived pointer
        // and the interface pointer are different addresses. Storing the
        // former into `void**` hands the plugin an object whose vtable is one
        // subobject off: every call lands on the wrong slot, and it dies
        // somewhere inside the plugin with no trace of where it came from.
        if (classId == Vst::IMessage::iid && interfaceId == Vst::IMessage::iid) {
            *obj = static_cast<Vst::IMessage*>(new HostMessage);
            return kResultOk;
        }
        if (classId == Vst::IAttributeList::iid && interfaceId == Vst::IAttributeList::iid) {
            *obj = static_cast<Vst::IAttributeList*>(new HostAttributeList);
            return kResultOk;
        }
        *obj = nullptr;
        return kResultFalse;
    }

    tresult PLUGIN_API isPlugInterfaceSupported(const TUID iid) override {
        // Only advertise optional plug-in interfaces this host genuinely uses.
        // Some controllers (notably instrument controllers) query this during
        // initialize and suppress their parameter/editor side when the host
        // context cannot answer the query at all.
        return FUID::fromTUID(iid) == Vst::IMidiMapping::iid ? kResultTrue
                                                             : kResultFalse;
    }
};

/// The one host context the whole process hands out.
///
/// A factory's `setHostContext` and a component's `initialize` both want an
/// `IHostApplication`, and a plugin may hold on to it, so it must outlive every
/// plugin — a function-local static of a non-destroyable type is exactly that.
inline Vst::IHostApplication* hostApplication() {
    static HostApplication application;
    // The cast is the `U::Implements` trap again: the most-derived pointer is
    // not the interface pointer, and handing the wrong one to a plugin sends
    // every call one vtable slot off.
    return static_cast<Vst::IHostApplication*>(&application);
}

/// One parameter's automation points inside a block.
class ParamValueQueue final : public U::Implements<U::Directly<Vst::IParamValueQueue>> {
public:
    struct Point { int32 offset; Vst::ParamValue value; };
    void reserve(std::size_t count) {
        if (m_storage.size() < count) m_storage.resize(count);
        m_points = m_storage; m_used = 0;
    }
    void reset(Vst::ParamID id) {
        m_id = id; m_points = m_storage; m_used = m_expected = 0;
    }
    void expectPoint() noexcept { ++m_expected; }
    std::size_t expectedPoints() const noexcept { return m_expected; }
    void useStorage(std::span<Point> storage) noexcept { m_points = storage; m_used = 0; }
    bool add(int32 offset, Vst::ParamValue value) {
        int32 index;
        return addPoint(offset, value, index) == kResultOk;
    }
    bool empty() const noexcept { return m_used == 0; }

    Vst::ParamID PLUGIN_API getParameterId() override { return m_id; }
    int32 PLUGIN_API getPointCount() override { return int32(m_used); }
    tresult PLUGIN_API getPoint(int32 index, int32& sampleOffset,
                                Vst::ParamValue& value) override {
        if (index < 0 || std::size_t(index) >= m_used) return kResultFalse;
        sampleOffset = m_points[std::size_t(index)].offset;
        value = m_points[std::size_t(index)].value;
        return kResultOk;
    }
    tresult PLUGIN_API addPoint(int32 sampleOffset, Vst::ParamValue value,
                                int32& index) override {
        // Several host edits at one sample have one audible final value.
        if (m_used && m_points[m_used - 1].offset == sampleOffset) {
            index = int32(m_used - 1); m_points[m_used - 1].value = value;
            return kResultOk;
        }
        if (m_used >= m_points.size()) { index = -1; return kResultFalse; }
        index = int32(m_used); m_points[m_used++] = {sampleOffset, value};
        return kResultOk;
    }

private:
    Vst::ParamID m_id = 0;
    std::vector<Point> m_storage;
    std::span<Point> m_points;
    std::size_t m_used = 0, m_expected = 0;
};

/// The block's parameter changes. Pre-grown in `activate`, cleared per block —
/// `process` must not allocate.
class ParameterChanges final : public U::Implements<U::Directly<Vst::IParameterChanges>> {
public:
    void reserve(std::size_t count, std::size_t pointCapacity = 0) {
        m_pointPool.resize(pointCapacity);
        m_queues.reserve(count);
        while (m_queues.size() < count) {
            auto queue = owned(new ParamValueQueue);
            queue->reserve(64);
            m_queues.push_back(std::move(queue));
        }
        // Preallocated open addressing: a block containing N changed
        // parameters must not perform N squared COM/linear-search operations.
        m_lookup.assign(std::bit_ceil(std::max(std::size_t{2}, m_queues.size() * 2)), 0);
        m_active.resize(m_queues.size());
        m_used = 0;
    }
    void reserveParameterPoints(Vst::ParamID id, std::size_t count) {
        auto found=std::find_if(m_special.begin(),m_special.end(),[&](const auto& q){return q.first==id;});
        if(found==m_special.end()){auto q=owned(new ParamValueQueue);q->reserve(count);m_special.emplace_back(id,std::move(q));}
        else found->second->reserve(count);
    }
    // The host counts this block's input points before filling the queues.
    // A single pool admits dense curves without reserving max-block storage
    // separately for every parameter the plugin exposes.
    bool preparePoints() noexcept {
        std::size_t used = 0;
        for (std::size_t i = 0; i < m_used; ++i) {
            const auto count = m_active[i]->expectedPoints() + 1; // editor value at sample zero
            if (count > m_pointPool.size() - used) return false;
            m_active[i]->useStorage(std::span(m_pointPool).subspan(used, count));
            used += count;
        }
        return true;
    }
    void clear() noexcept {
        if (!m_used) return; // Idle plugins need no table scan on each audio block.
        m_used = 0;
        std::fill(m_lookup.begin(), m_lookup.end(), 0);
    }

    /// Null when the pre-grown pool is exhausted, which is a dropped automation
    /// point rather than an allocation on the audio thread.
    ParamValueQueue* begin(Vst::ParamID id) {
        int32 index = -1;
        return findOrAdd(id, index);
    }

    int32 PLUGIN_API getParameterCount() override { return int32(m_used); }
    Vst::IParamValueQueue* PLUGIN_API getParameterData(int32 index) override {
        if (index < 0 || std::size_t(index) >= m_used) return nullptr;
        return m_active[std::size_t(index)];
    }
    Vst::IParamValueQueue* PLUGIN_API addParameterData(const Vst::ParamID& id,
                                                       int32& index) override {
        return findOrAdd(id, index);
    }

private:
    ParamValueQueue* findOrAdd(Vst::ParamID id, int32& index) noexcept {
        index = -1;
        if (m_lookup.empty()) return nullptr;
        uint32 hash = id;
        hash ^= hash >> 16; hash *= 0x7feb352du; hash ^= hash >> 15;
        const auto mask = m_lookup.size() - 1;
        auto slot = std::size_t(hash) & mask;
        while (const auto encoded = m_lookup[slot]) {
            const auto existing = encoded - 1;
            if (m_active[existing]->getParameterId() == id) {
                index = int32(existing); return m_active[existing];
            }
            slot = (slot + 1) & mask;
        }
        if (m_used >= m_queues.size()) return nullptr;
        index = int32(m_used++);
        m_lookup[slot] = uint32(index) + 1;
        auto special=std::find_if(m_special.begin(),m_special.end(),[&](const auto& q){return q.first==id;});
        m_active[index]=special==m_special.end()?m_queues[index].get():special->second.get();
        m_active[index]->reset(id);
        return m_active[index];
    }
    std::vector<ParamValueQueue::Point> m_pointPool;
    std::vector<IPtr<ParamValueQueue>> m_queues;
    std::vector<ParamValueQueue*> m_active;
    std::vector<std::pair<Vst::ParamID,IPtr<ParamValueQueue>>> m_special;
    std::vector<uint32> m_lookup;
    std::size_t m_used = 0;
};

/// The block's notes. Pre-grown in `activate`, cleared per block — `process`
/// must not allocate. It is handed over even when empty, because plenty of
/// plugins dereference the list without checking it for null.
class EventList final : public U::Implements<U::Directly<Vst::IEventList>> {
public:
    void reserve(std::size_t count) { m_events.reserve(count); }
    void clear() noexcept { m_events.clear(); }
    bool full() const noexcept { return m_events.size() >= m_events.capacity(); }

    int32 PLUGIN_API getEventCount() override { return int32(m_events.size()); }
    tresult PLUGIN_API getEvent(int32 index, Vst::Event& event) override {
        if (index < 0 || std::size_t(index) >= m_events.size()) return kResultFalse;
        event = m_events[std::size_t(index)];
        return kResultOk;
    }
    tresult PLUGIN_API addEvent(Vst::Event& event) override {
        if (full()) return kResultFalse;
        m_events.push_back(event);
        return kResultOk;
    }

private:
    std::vector<Vst::Event> m_events;
};

/// The host side of a plugin's editor window: the plugin calls `resizeView`
/// when it wants to change size.
class PlugFrame final : public U::ImplementsNonDestroyable<U::Directly<IPlugFrame>> {
public:
    std::function<bool(int32 width, int32 height)> onResize;

    tresult PLUGIN_API resizeView(IPlugView* view, ViewRect* newSize) override {
        if (!view || !newSize) return kInvalidArgument;
        if (!onResize || !onResize(newSize->getWidth(), newSize->getHeight())) {
            return kResultFalse;
        }
        // The plugin only redraws once the view itself is told, and the host
        // has to do that after its own window has taken the new size.
        view->onSize(newSize);
        return kResultOk;
    }
};

/// What the plugin's editor calls when the user moves something in it.
class ComponentHandler final
    : public U::ImplementsNonDestroyable<
          U::Directly<Vst::IComponentHandler, Vst::IComponentHandler2>> {
public:
    std::function<void(Vst::ParamID, bool begin)> onGesture;
    std::function<void(Vst::ParamID, Vst::ParamValue normalized)> onEdit;
    std::function<void(int32 flags)> onRestart;

    tresult PLUGIN_API beginEdit(Vst::ParamID id) override {
        if (onGesture) onGesture(id, true);
        return kResultOk;
    }
    tresult PLUGIN_API performEdit(Vst::ParamID id, Vst::ParamValue normalized) override {
        if (onEdit) onEdit(id, normalized);
        return kResultOk;
    }
    tresult PLUGIN_API endEdit(Vst::ParamID id) override {
        if (onGesture) onGesture(id, false);
        return kResultOk;
    }
    tresult PLUGIN_API restartComponent(int32 flags) override {
        if (onRestart) onRestart(flags);
        return kResultOk;
    }

    // IComponentHandler2 is optional, but widely used by preset browsers and
    // modern instrument editors. Grouping is already represented by the
    // begin/end callbacks above; dirty state is persisted whenever the project
    // is saved. Opening an editor is initiated by our UI, so that one request
    // remains unsupported instead of pretending an invisible window opened.
    tresult PLUGIN_API setDirty(TBool) override { return kResultOk; }
    tresult PLUGIN_API requestOpenEditor(FIDString) override { return kNotImplemented; }
    tresult PLUGIN_API startGroupEdit() override { return kResultOk; }
    tresult PLUGIN_API finishGroupEdit() override { return kResultOk; }
};

} // namespace daw::plugins::vst3
