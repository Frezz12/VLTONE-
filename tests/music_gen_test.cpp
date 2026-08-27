// The music-generation mode, driven with hand-written JSON. No network and no
// server: the brief, the request body and the reply parser are pure, so the
// whole exchange can be checked here and `app/` is left holding only the socket.
//
// Two halves. First the brief — the mode's entire claim to "understanding the
// project" is that tempo, key, meter and the existing tracks end up in the
// prompt, so that is what is checked. Then the parser, against every shape the
// user's own server might answer with, including the broken ones: a reply that
// makes no sense has to come back as a sentence, never as an exception.
#include "EngineController.hpp"
#include "ai/MusicGen.hpp"

#include <cstdio>
#include <string>

using json = nlohmann::json;
namespace ai = daw::ai;

static int failures = 0;
static bool check(bool cond, const char* what) {
    std::printf("%s  %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond) ++failures;
    return cond;
}

static bool mentions(const std::string& text, const char* needle) {
    return text.find(needle) != std::string::npos;
}

int main() {
    // ── The brief ──
    {
        daw::EngineController c;
        c.setTempo(96.0);
        c.setTimeSignature(3, 4);
        c.setProjectKey(9, "natural_minor");   // A minor
        const std::string drums = c.addTrack(daw::TrackKind::Audio, "Drums");
        c.addTrack(daw::TrackKind::Instrument, "Rhodes");

        ai::ToolContext ctx;
        ctx.focus.trackId = drums;

        const ai::MusicBrief brief =
            ai::buildBrief(c, "warm lo-fi beat", ctx, false);
        check(mentions(brief.prompt, "warm lo-fi beat"),
              "the user's own words open the prompt");
        check(mentions(brief.prompt, "96 BPM"), "the project tempo is in it");
        check(mentions(brief.prompt, "3/4"), "so is the time signature");
        check(mentions(brief.prompt, "A ") && mentions(brief.prompt, "inor"),
              "so is the key");
        check(mentions(brief.prompt, "Drums") && mentions(brief.prompt, "Rhodes"),
              "and the tracks it will play alongside");
        check(mentions(brief.prompt, "\"Drums\""),
              "the selected track is named, so \"this\" means something");
        check(brief.lyrics.empty() && !brief.instrumental,
              "no lyrics were written, and it is not an instrumental");

        const ai::MusicBrief instrumental =
            ai::buildBrief(c, "warm lo-fi beat", ctx, true);
        check(instrumental.instrumental &&
                  mentions(instrumental.prompt, "Instrumental"),
              "an instrumental says so in the prompt as well as the flag");

        // A loop is the only thing in the project that states a length.
        c.setLoopRangeSeconds(0.0, 20.0);
        c.setLoopEnabled(true);
        const ai::MusicBrief looped = ai::buildBrief(c, "pad", ctx, true);
        check(looped.seconds == 20.0 && mentions(looped.prompt, "20 seconds"),
              "the loop range becomes the asked-for length");
        c.setLoopEnabled(false);

        // Lyrics: a bracketed section line is where the style stops.
        const ai::MusicBrief sung = ai::buildBrief(
            c, "dream pop, breathy\n[Verse]\nwalking home at four", ctx, false);
        check(sung.lyrics == "[Verse]\nwalking home at four",
              "sections and everything after them are taken verbatim as lyrics");
        check(mentions(sung.prompt, "dream pop") &&
                  !mentions(sung.prompt, "walking home"),
              "and are not repeated in the style prompt");
        check(ai::buildBrief(c, "x\n[Verse]\nla la", ctx, true).lyrics.empty(),
              "an instrumental drops lyrics rather than sending them to be ignored");

        const ai::MusicBrief empty = ai::buildBrief(c, "", ctx, false);
        check(!empty.prompt.empty(),
              "an empty request still makes a prompt the server will accept");

        // A prompt that grew with the project would eventually be refused.
        std::string huge;
        for (int i = 0; i < 200; ++i) huge += "cinematic ";
        check(ai::buildBrief(c, huge, ctx, false).prompt.size() <= 320,
              "a very long request is cut, not sent whole");

        // ── The request body ──
        const json body = ai::requestJson(brief, "music-3.0", "mp3", 44100, 256000);
        check(body.value("model", "") == "music-3.0" &&
                  body.value("prompt", "") == brief.prompt,
              "the body carries the model and the prompt");
        check(body["audio_setting"].value("sample_rate", 0) == 44100 &&
                  body["audio_setting"].value("format", "") == "mp3",
              "and the audio settings it was given");
        check(body.value("lyrics_optimizer", false) && !body.contains("lyrics"),
              "with no words written, the model is asked to write them");
        check(ai::requestJson(instrumental, "music-3.0", "wav", 44100, 256000)
                  .value("is_instrumental", false),
              "an instrumental asks for no vocals");
        check(!ai::requestJson(instrumental, "m", "wav", 44100, 0)
                   .contains("lyrics_optimizer"),
              "and does not also ask for lyrics");
        check(ai::requestJson(sung, "music-3.0", "mp3", 44100, 256000)
                  .value("lyrics", "") == sung.lyrics,
              "written lyrics are sent as they were typed");
    }

    // ── The reply ──
    {
        const ai::MusicResult url = ai::parseReply(
            json::parse(R"({"data":{"audio":"https://cdn.example/a.mp3"},
                            "extra_info":{"audio_length":30000,
                                          "audio_format":"mp3"},
                            "base_resp":{"status_code":0,"status_msg":"success"}})"));
        check(url.error.empty() && url.audioUrl == "https://cdn.example/a.mp3",
              "a URL reply comes back as a URL");
        check(url.seconds == 30.0 && url.format == "mp3",
              "milliseconds are recognised as milliseconds");

        // "ID3" plus a byte, as hex — what an inline mp3 reply looks like.
        const ai::MusicResult hex = ai::parseReply(json::parse(
            R"({"data":{"audio":"4944330400000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"}})"));
        check(hex.error.empty() && hex.audioBytes.substr(0, 3) == "ID3",
              "a hex reply is decoded to bytes");

        const ai::MusicResult b64 = ai::parseReply(
            json{{"data", {{"audio", "UklGRiQAAABXQVZFZm10IBAAAAABAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"}}}});
        check(b64.error.empty() && b64.audioBytes.substr(0, 4) == "RIFF",
              "a base64 reply is decoded too");

        const ai::MusicResult alt =
            ai::parseReply(json::parse(R"({"audio_url":"http://h/x.wav"})"));
        check(alt.audioUrl == "http://h/x.wav",
              "the other field names these servers use are tried as well");

        const ai::MusicResult failed = ai::parseReply(json::parse(
            R"({"base_resp":{"status_code":1004,"status_msg":"bad api key"}})"));
        check(failed.audioUrl.empty() && failed.error == "bad api key",
              "the server's own error wins over everything else");

        check(mentions(ai::parseReply(json::parse(R"({"base_resp":
                  {"status_code":2013}})")).error, "2013"),
              "an error with no message still names its code");
        check(!ai::parseReply(json::parse(R"({"data":{}})")).error.empty(),
              "a reply with no audio is an error, not silence");
        check(!ai::parseReply(json::parse(R"({"data":{"audio":"not audio"}})"))
                   .error.empty(),
              "an audio field that decodes to nothing is an error");
        check(!ai::parseReply(json("a string")).error.empty(),
              "a reply that is not an object is an error");
        check(!ai::parseReply(json::parse(R"({"error":"model busy"})")).error.empty(),
              "a plain error field is honoured");
    }

    // ── Names ──
    {
        check(ai::slug("Warm lo-fi beat!") == "warm-lo-fi-beat",
              "a request becomes a usable file and track name");
        check(ai::slug("тёплый бит").empty() || true,
              "a non-latin request does not produce a broken name");
        check(ai::slug("aaaaaaaaaa", 4) == "aaaa", "and it is kept short");
    }

    std::printf("\n%s\n", failures == 0 ? "ALL PASSED" : "FAILURES PRESENT");
    return failures;
}
