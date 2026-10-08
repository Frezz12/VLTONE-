#pragma once

#include "AudioRuntime.hpp"
#include "AudioValueCodec.hpp"
#include "AudioRuntimeCoreFields.hpp"

#include <functional>
#include <tuple>
#include <type_traits>

namespace daw::audio_rpc {

enum class Method : std::uint32_t {
#define DAW_AUDIO_METHOD(id, name) name = id,
#include "AudioRuntimeMethods.def"
#undef DAW_AUDIO_METHOD
    openPluginEditor = 300, showDeviceControlPanel,
    startMiniModuleCompile, pollMiniModuleCompile, forgetMiniModuleCompile,
    readoutBatch = 305
};

namespace detail {
template<class T> struct OwnedType { using type = std::remove_cvref_t<T>; };
template<class T, std::size_t N> struct OwnedType<std::span<T, N>> { using type = std::vector<std::remove_const_t<T>>; };
template<> struct OwnedType<std::string_view> { using type = std::string; };
template<class T> using Owned = typename OwnedType<std::remove_cvref_t<T>>::type;
template<class T> using ResultValue = std::conditional_t<std::is_same_v<T, audio::Result>, AudioResultValue,
    std::conditional_t<std::is_void_v<T>, std::tuple<>, T>>;

template<class T> inline constexpr bool mutableSpan = false;
template<class T, std::size_t N> inline constexpr bool mutableSpan<std::span<T, N>> = !std::is_const_v<T>;
template<class T> inline constexpr bool output = mutableSpan<T> ||
    (std::is_lvalue_reference_v<T> && !std::is_const_v<std::remove_reference_t<T>>);

template<class T> using Output = std::conditional_t<output<T>, std::tuple<Owned<T>>, std::tuple<>>;
template<class T> decltype(auto) view(Owned<T>& value) {
    if constexpr (requires { typename std::remove_cvref_t<T>::element_type; } &&
                  !std::is_same_v<std::remove_cvref_t<T>, Owned<T>>) return T(value);
    else if constexpr (std::is_same_v<T, std::string_view>) return std::string_view(value);
    else return (value);
}
template<class T> Output<T> copyOutput(const Owned<T>& value) {
    if constexpr (output<T>) return {value};
    else return {};
}
template<class T> struct Signature;
template<class R, class... Args> struct Signature<R (AudioRuntime::*)(Args...)> {
    using Result = ResultValue<R>;
    using Arguments = std::tuple<Args...>;
    using Inputs = std::tuple<Owned<Args>...>;
    using Outputs = decltype(std::tuple_cat(std::declval<Output<Args>>()...));
    using Reply = std::tuple<Result, Outputs>;
    template<auto Function> static Reply invoke(AudioRuntime& runtime, Inputs& values) {
        return invoke<Function>(runtime, values, std::index_sequence_for<Args...>{});
    }
private:
    template<auto Function, std::size_t... I> static Reply invoke(AudioRuntime& runtime, Inputs& values,
                                                                 std::index_sequence<I...>) {
        Result result{};
        if constexpr (std::is_void_v<R>) std::invoke(Function, runtime, view<Args>(std::get<I>(values))...);
        else result = Result(std::invoke(Function, runtime, view<Args>(std::get<I>(values))...));
        return {std::move(result), std::tuple_cat(copyOutput<Args>(std::get<I>(values))...)};
    }
};
template<class R, class... Args> struct Signature<R (AudioRuntime::*)(Args...) const>
    : Signature<R (AudioRuntime::*)(Args...)> {};
}

template<Method> struct Binding;
#define DAW_AUDIO_METHOD(id, name) \
    template<> struct Binding<Method::name> : detail::Signature<decltype(&AudioRuntime::name)> { \
        static constexpr auto function = &AudioRuntime::name; \
    };
#include "AudioRuntimeMethods.def"
#undef DAW_AUDIO_METHOD

/// Runs only on the child's control thread. The method table reuses runtime
/// behavior; serialization contains owned values and numeric identities only.
std::vector<std::uint8_t> dispatch(AudioRuntime& runtime, Method method,
    std::span<const std::uint8_t> payload, const std::filesystem::path& inputDirectory,
    ProcessAudioResources::Cache& inputCache, ProcessAudioResources& outputResources);

} // namespace daw::audio_rpc
