// Attachment Tweaks -- tools for any attachment, as plugins\mt_plugin_AttachmentTweaks.dll: per-section
// light control and the placement clipboard. Works on vanilla attachments; MoreAttachments' parts carry
// the labelled sections it drives, and neither needs the other.
//
// An attachment can carry several light controls (MTLightControl, MTLightControl1,
// ...) and labelled menu lines per section: "Toggle <S>", "<S> Mode", "<S> Color".
// The game itself applies any light interaction to every control on the actor and
// keeps one mode per attachment, natively, where nothing can read it. So this plugin
// keeps the per-section state itself:
//
//   - which line was clicked: the hovered menu button's label at a mouse click / Enter
//     (Widget.IsHovered / HasAnyUserFocus, KismetTextLibrary.Conv_TextToString)
//   - Toggle flips that section only; Color adopts the colour the picker applies, for
//     that section only; Mode stores the option picked from the submenu for that section
//   - every tick each section's light is set from its own state: Manual = its toggle,
//     otherwise it follows the vehicle (brake pedal, reverse gear, turn signal, headlight
//     mode, engine, interior light, siren) -- overriding what the game applied to all
//
// The placement window gets a Clipboard section under the coords (COPY, PASTE, MIRROR), then a
// Lights section title over the part's own options. COPY puts the location, rotation
// and scale rows on the clipboard as text, PASTE sets them on the part being placed, and MIRROR
// does the same flipped left to right. A row is written, then stepped up and down: the steps are
// what makes the game move the part. Save still commits it.
//
// Settings persist in Binaries\Win64\more_attachments.ini, keyed by Net_VehicleId and the part slot, and
// are also put on the car: one entry per section in its customisation list (as ECU Tuning's tune and
// Custom Plates' design are), named QXA<part slot>S<section>M<mode><L lit|D dark>, its colour the
// section's. The game saves that list only now and then, so the ini is what lasts. An attachment
// without labelled sections is left alone, and
// without this plugin the labelled lines still work -- they just drive every section.
//
// shortcut: runs where the light state is authoritative (single player, or the host). On
// a dedicated server the client's writes are overwritten by replication -- running it on
// the server would need the click (which only the client sees) forwarded, not done here.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>
#include "../include/mt_plugin.hpp"

using namespace mt;

enum Mode { Manual, Headlight, HighBeam, Reverse, Interior, Brake, TurnLeft, TurnRight, Siren, EngineOn, Hazard };
static const char* kModeNames[] = {"Manual", "Headlight", "HighBeam", "Reverse", "Interior", "Brake",
                                   "TurnLeft", "TurnRight", "Siren", "EngineOn", "Hazard"};

// Submenu option label -> mode. Order matters: "Left Turn Signal" before anything generic.
static int modeOf(const std::string& label) {
    std::string s;
    for (char c : label) s += (char)tolower((unsigned char)c);
    struct { const char* k; int m; } map[] = {
        {"left", TurnLeft}, {"right", TurnRight}, {"hazard", Hazard}, {"high", HighBeam},
        {"head", Headlight}, {"reverse", Reverse}, {"interior", Interior}, {"brake", Brake},
        {"siren", Siren}, {"engine", EngineOn}, {"manual", Manual}};
    for (auto& e : map) if (s.find(e.k) != std::string::npos) return e.m;
    return -1;
}

struct Ref { int index; uintptr_t ptr; };
static bool alive(const Ref& r) { return r.ptr && api->object(r.index) == r.ptr; }

struct Section {
    std::string name; Ref ctl{}; bool on = false; float color[4] = {1, 1, 1, 1}; int mode = Manual;
    DWORD lastTrue = 0;  // follow modes: last time the signal was on (off-delay)
};
struct Fitted {
    Ref actor, interactable;
    std::vector<Ref> controls;  // by name: MTLightControl, MTLightControl1, ...
    std::vector<Section> sec;
    bool parsed = false, loaded = false, detachLogged = false;
    int compLights = 0;  // how many light controls the vehicle's list held before we emptied it
    std::string key;            // for the log and the old ini, once the vehicle is known
    int slot = -1;              // its part slot on the vehicle: what its entries on the car are keyed by
    DWORD dirtyAt = 0;          // changed, not yet on the car
    uintptr_t vehicle = 0, comp = 0;  // comp: the vehicle's MTVehicleAttachmentPartComponent for it
};

// ---- reflection, resolved once ----
static uintptr_t cCtl, cInter, cBtn, cList, cVeh, cInterior, cAttComp, cPC;
static uintptr_t fHovered, fFocus, fToStr, textCDO, fRepOn, fRepColor, fIsVisible, gameplayCDO;
static int oOn, oColor, oParams, pSize, pName, oBtnName, oInText, oRet, oHighlight, oListInters;
static int oVehId, oBrake, oCold, oTrans, oHot, oInteriors, cTurn, cHead, cSiren, tGear, hRpm, iOn;
static int oOwner, oParentComp, oSlot, oCompLights, oReverseLights, oIntensity, oPCPlayer, oPawn;
static uintptr_t cPlace, widgetCDO, fCreate, fAddChild, fGetParent, fWidgetVisible, fToText, fSetText, fSetRich, fPadding, fUp, fDown, fChildIndex, fChildCount, fChildAt, fRemove, fSetVis;
static int oPlaceOptions, oPlaceAttachment, oPlaceSave, oPlaceScale, oPlaceRows[7], oRowSpin, oSpinValue = -1, oBlock, oRich, pToIn, pToRet;
static void (__fastcall* spinSet)(uintptr_t self, float value);
static const char* const kPlaceRows[7] = {"LocationXSetting", "LocationYSetting", "LocationZSetting", "RotationXSetting",
                                          "RotationYSetting", "RotationZSetting", "ScaleSetting"};
static const wchar_t* const kTools[3] = {L"COPY", L"PASTE", L"MIRROR"};
static const wchar_t* const kTitles[2] = {L"Clipboard", L"Lights"};  // over the buttons, over the part's own options
static const DWORD kOffDelay = 250;  // ms a follow signal must stay off before the light goes off

static std::mutex lock;
static std::vector<Ref> buttons, lists, controllers, places;
static Ref tools[3], titles[2], placing;
static uintptr_t toolsIn;  // the window the buttons were built in: reopening the editor makes a new one
static uintptr_t cTitle;
static int oTitleText = -1, oTitleBlock = -1;
static int toolLabels;
static std::map<uintptr_t, Fitted> fitted;  // actor -> state
static std::map<uintptr_t, std::string> labelCache;  // FText data ptr -> label (Conv_TextToString leaks a string)

// kind: 'm' mode, 'c' colour. seen: every colour on the actor's controls at click time --
// the game keeps re-applying its own stored colour, so only a colour new since the click
// is the one the picker set.
struct Pending { uintptr_t actor = 0; std::vector<int> secs; char kind = 0; DWORD at = 0; std::vector<std::array<float, 3>> seen; } pending;
static bool mouseWasDown, enterWasDown;

static std::string toStr(uintptr_t ftext) {
    uintptr_t data = get<uintptr_t>(ftext);
    if (!data) return "";
    auto it = labelCache.find(data);
    if (it != labelCache.end()) return it->second;
    unsigned char parms[0x40] = {0};
    rd(ftext, parms + oInText, oRet - oInText);  // FText is the gap to ReturnValue (0x10 on UE 5.5)
    api->process_event(textCDO, fToStr, parms);
    uintptr_t s = 0; int len = 0;
    memcpy(&s, parms + oRet, 8); memcpy(&len, parms + oRet + 8, 4);
    std::string out;
    if (s && len > 0 && len < 256) {
        std::vector<wchar_t> w(len);
        if (rd(s, w.data(), (size_t)len * 2)) for (int i = 0; i + 1 < len; i++) out += w[i] < 128 ? (char)w[i] : '?';
    }
    return labelCache[data] = out;
}

static bool callBool(uintptr_t obj, uintptr_t fn) {
    unsigned char parms[16] = {0};
    api->process_event(obj, fn, parms);
    return parms[0] != 0;
}

// "Toggle Brake Light" -> ('t', "Brake Light"); "Brake Light Mode" -> ('m', ...); "... Color" -> ('c', ...)
static char split(const std::string& label, std::string& name) {
    auto ends = [&](const char* suf) { size_t n = strlen(suf); return label.size() > n && label.compare(label.size() - n, n, suf) == 0; };
    if (label.rfind("Toggle ", 0) == 0) { name = label.substr(7); return 't'; }
    if (ends(" Mode")) { name = label.substr(0, label.size() - 5); return 'm'; }
    if (ends(" Color")) { name = label.substr(0, label.size() - 6); return 'c'; }
    return 0;
}

// Sections from the labelled InteractionParams, in order; section i drives control i.
static void parse(Fitted& f) {
    f.parsed = true;
    if (!alive(f.interactable)) return;
    uintptr_t data = get<uintptr_t>(f.interactable.ptr + oParams); int n = get<int>(f.interactable.ptr + oParams + 8);
    for (int i = 0; i < n && i < 64; i++) {
        std::string name;
        char kind = split(toStr(data + (uintptr_t)i * pSize + pName), name);
        if (kind != 't' && kind != 'm') continue;  // a Color line may name a group ("Signal Color")
        bool known = false;
        for (auto& s : f.sec) known |= s.name == name;
        if (!known) { Section s; s.name = name; f.sec.push_back(s); }
    }
    if (f.sec.size() < 2 || f.sec.size() > f.controls.size()) { f.sec.clear(); return; }  // not ours
    for (size_t i = 0; i < f.sec.size(); i++) {
        f.sec[i].ctl = f.controls[i];
        f.sec[i].on = get<uint8_t>(f.controls[i].ptr + oOn) & 1;
        rd(f.controls[i].ptr + oColor, f.sec[i].color, 16);
    }
    MT_LOG("%s: %d sections", objName(classOf(f.actor.ptr)).c_str(), (int)f.sec.size());
}

// The sections a line names: an exact section, else every section whose name ends in
// " <name>" -- so "Signal Color" covers "Left Signal" and "Right Signal".
static std::vector<int> groupOf(const Fitted& f, const std::string& name) {
    std::vector<int> out;
    for (size_t i = 0; i < f.sec.size(); i++) if (f.sec[i].name == name) return {(int)i};
    for (size_t i = 0; i < f.sec.size(); i++) {
        const std::string& n = f.sec[i].name;
        if (n.size() > name.size() + 1 && n.compare(n.size() - name.size(), name.size(), name) == 0
            && n[n.size() - name.size() - 1] == ' ') out.push_back((int)i);
    }
    return out;
}

// ---- on the car: its customisation list (see the top) ----
static uintptr_t fGetPC, fSendCustom, stringCDO, fToName;
static int oCustom = -1, cColors, cSlot, cColor, cSize, customSize, pSendVehicle, pSendCustom, pNameIn, pNameRet;

static uint64_t nameOf(const char* text) {
    wchar_t w[64];
    swprintf_s(w, L"%hs", text);
    unsigned char p[0x40] = {0};
    struct { const wchar_t* data; int num, max; } s = {w, (int)wcslen(w) + 1, (int)wcslen(w) + 1};
    memcpy(p + pNameIn, &s, sizeof s);
    api->process_event(stringCDO, fToName, p);
    uint64_t n = 0;
    memcpy(&n, p + pNameRet, 8);
    return n;
}

// An entry's slot, section, mode and light; false for anything not ours.
static bool entryOf(uintptr_t e, int& slot, int& sec, int& mode, bool& on) {
    char name[64] = {0}, lit = 0;
    if (!api->fname(e + cSlot, name, sizeof name)) return false;
    if (sscanf_s(name, "QXA%dS%dM%d%c", &slot, &sec, &mode, &lit, 1) != 4 || (lit != 'L' && lit != 'D')) return false;
    on = lit == 'L';
    return mode >= 0 && mode <= Hazard;
}

static bool readCar(Fitted& f) {
    if (oCustom < 0 || f.slot < 0) return false;
    uintptr_t d = get<uintptr_t>(f.vehicle + oCustom + cColors);
    int n = get<int>(f.vehicle + oCustom + cColors + 8);
    bool got = false;
    for (int i = 0; d && i < n && i < 200; i++) {
        uintptr_t e = d + (uintptr_t)i * cSize;
        int slot, sec, mode; bool on;
        if (!entryOf(e, slot, sec, mode, on) || slot != f.slot || sec < 0 || sec >= (int)f.sec.size()) continue;
        uint32_t c = get<uint32_t>(e + cColor);
        Section& s = f.sec[sec];
        s.on = on; s.mode = mode;
        s.color[0] = ((c >> 16) & 255) / 255.0f; s.color[1] = ((c >> 8) & 255) / 255.0f; s.color[2] = (c & 255) / 255.0f;
        got = true;
    }
    return got;
}

// The list goes back whole: every other entry as it was (ECU Tuning's tune, Custom Plates' design,
// other attachments' sections), then this attachment's. Sent only when they differ.
static void writeCar(const Fitted& f) {
    if (oCustom < 0 || f.slot < 0 || !f.vehicle) return;
    static unsigned char list[0x40 * 220];
    uintptr_t d = get<uintptr_t>(f.vehicle + oCustom + cColors);
    int n = get<int>(f.vehicle + oCustom + cColors + 8), have = 0, same = 0;
    if (n > 200 || cSize > 0x40) return;
    for (int i = 0; d && i < n; i++) {
        uintptr_t e = d + (uintptr_t)i * cSize;
        int slot, sec, mode; bool on;
        if (entryOf(e, slot, sec, mode, on) && slot == f.slot) {
            if (sec >= 0 && sec < (int)f.sec.size()) {
                const Section& s = f.sec[sec];
                uint32_t c = get<uint32_t>(e + cColor) & 0xFFFFFF, want = 0;
                for (int k = 0; k < 3; k++) want = (want << 8) | (uint32_t)(fminf(fmaxf(s.color[k], 0), 1) * 255 + 0.5f);
                same += mode == s.mode && on == s.on && c == want;
            }
            continue;
        }
        rd(e, list + (uintptr_t)have * cSize, cSize);
        have++;
    }
    if (same == (int)f.sec.size()) return;
    for (size_t i = 0; i < f.sec.size(); i++) {
        const Section& s = f.sec[i];
        char name[48];
        sprintf_s(name, "QXA%dS%dM%d%c", f.slot, (int)i, s.mode, s.on ? 'L' : 'D');
        uint64_t fn = nameOf(name);
        uint32_t color = 0xFF000000u;
        for (int k = 0; k < 3; k++) color |= (uint32_t)(fminf(fmaxf(s.color[k], 0), 1) * 255 + 0.5f) << (16 - 8 * k);
        unsigned char* e = list + (uintptr_t)have * cSize;
        memset(e, 0, cSize);
        memcpy(e + cSlot, &fn, 8);
        memcpy(e + cColor, &color, 4);
        have++;
    }
    struct { uintptr_t context; int index; uintptr_t ret; } pc = {f.vehicle, 0, 0};
    api->process_event(gameplayCDO, fGetPC, &pc);
    if (!pc.ret) return;
    unsigned char q[0x40] = {0};
    memcpy(q + pSendVehicle, &f.vehicle, 8);
    rd(f.vehicle + oCustom, q + pSendCustom, customSize);
    struct { void* data; int num, max; } colors = {list, have, have};
    memcpy(q + pSendCustom + cColors, &colors, sizeof colors);
    api->process_event(pc.ret, fSendCustom, q);
    MT_LOG("%s: %d sections saved on the car", f.key.c_str(), (int)f.sec.size());
}

// Kept in more_attachments.ini (what persists: the game saves a car's customisation list only now and
// then, not as it changes) and put on the car too. The car copy is sent a second after the last change,
// as the colour picker previews every frame.
static void save(Fitted& f) {
    f.dirtyAt = GetTickCount() | 1;
    if (f.key.empty()) return;
    std::wstring k(f.key.begin(), f.key.end());
    for (auto& s : f.sec) {
        char v[96];
        sprintf_s(v, "%d %.4f %.4f %.4f %s", s.on ? 1 : 0, s.color[0], s.color[1], s.color[2], kModeNames[s.mode]);
        std::wstring n(s.name.begin(), s.name.end()), vv(v, v + strlen(v));
        iniSet(L"more_attachments.ini", k.c_str(), n.c_str(), vv.c_str(), true);  // at once, so a crash cannot lose it
    }
}

static void load(Fitted& f) {
    f.loaded = true;
    std::wstring k(f.key.begin(), f.key.end());
    bool any = false;
    for (auto& s : f.sec) {
        std::wstring n(s.name.begin(), s.name.end());
        std::wstring b = iniGet(L"more_attachments.ini", k.c_str(), n.c_str());
        if (b.empty()) continue;
        int on = 0; float r, g, bl; wchar_t m[24] = {0};
        if (swscanf_s(b.c_str(), L"%d %f %f %f %23s", &on, &r, &g, &bl, m, 24) >= 4) {
            s.on = on != 0; s.color[0] = r; s.color[1] = g; s.color[2] = bl;
            std::string ms(m, m + wcslen(m));
            for (int i = 0; i <= Hazard; i++) if (ms == kModeNames[i]) s.mode = i;
            any = true;
        }
    }
    if (!any && readCar(f)) { any = true; save(f); }  // only on the car (this session's): into the ini too
    else if (any) f.dirtyAt = GetTickCount() | 1;   // and onto the car, for this session
    MT_LOG("%s: %s", f.key.c_str(), any ? "settings loaded" : "no settings yet");
}

// The vehicle this attachment rides on, and the ini key (vehicle id + part slot).
static void bindVehicle(Fitted& f) {
    int slot = -1;
    int wIndex = get<int>(f.actor.ptr + oParentComp);  // FWeakObjectPtr: ObjectIndex, SerialNumber
    uintptr_t comp = wIndex > 0 ? api->object(wIndex) : 0;
    if (comp && api->is_a(classOf(comp), cAttComp)) slot = get<uint16_t>(comp + oSlot);
    uintptr_t owner = get<uintptr_t>(f.actor.ptr + oOwner);
    if ((!owner || !api->is_a(classOf(owner), cVeh)) && comp) owner = outerOf(comp);  // the component lives on the vehicle
    if (!owner || !api->is_a(classOf(owner), cVeh)) return;
    // Net_VehicleId <= 0: the garage's preview copy (id -1) or a vehicle not yet
    // registered -- built and torn down by the UI; writing into one crashed the game.
    if (get<long long>(owner + oVehId) <= 0) return;
    char k[96];
    sprintf_s(k, "veh%lld_slot%d_%s", get<long long>(owner + oVehId), slot, objName(classOf(f.actor.ptr)).c_str());
    f.vehicle = owner;
    f.comp = comp && api->is_a(classOf(comp), cAttComp) ? comp : 0;
    f.slot = slot;
    f.key = k;
}

static bool near3(const float* a, const float* b) {
    return fabsf(a[0] - b[0]) < 1e-3f && fabsf(a[1] - b[1]) < 1e-3f && fabsf(a[2] - b[2]) < 1e-3f;
}

// The vehicle drives an attachment's lights through its part component's LightControls
// (that is how the game's one-mode-per-attachment reaches every section). Emptying that
// list leaves this plugin the only writer, which is what stops the per-frame tug of war
// (the reverse flicker). Only Num is zeroed -- the allocation stays the game's, untouched.
//
// The game's colour editor works through that same list, so while a colour pick is open
// on this attachment the list is put back (same count, contents never touched) and taken
// away again once the picked colour has been adopted.
static void detach(Fitted& f, bool picking) {
    if (!f.comp || oCompLights < 0) return;
    int n = get<int>(f.comp + oCompLights + 8);
    if (picking) {
        if (n == 0 && f.compLights > 0) wr(f.comp + oCompLights + 8, &f.compLights, 4);
        return;
    }
    if (n <= 0) return;
    f.compLights = n;
    int zero = 0;
    wr(f.comp + oCompLights + 8, &zero, 4);
    if (!f.detachLogged) { f.detachLogged = true; MT_LOG("%s: took %d light controls off the vehicle's list", f.key.c_str(), n); }
}

static bool follows(uintptr_t v, int mode, DWORD now) {
    uint8_t turn = get<uint8_t>(v + oCold + cTurn), head = get<uint8_t>(v + oCold + cHead);
    bool phase = (now % 800) < 400;  // shortcut: own 0.8 s blink, not synced to the car's blinker clock
    switch (mode) {
        case Headlight: return head >= 1;
        case HighBeam:  return head >= 2;
        case Reverse: {
            // CurrentGear is a position in the gearbox's list (reverses, N, forwards),
            // not a signed gear, so mirror the car's own reverse lamps instead: a lamp
            // that is both visible and lit means the car is reversing.
            uintptr_t d = get<uintptr_t>(v + oReverseLights); int n = get<int>(v + oReverseLights + 8);
            bool on = false;
            for (int i = 0; i < n && i < 8 && !on; i++) {
                uintptr_t l = get<uintptr_t>(d + i * 8);
                if (!l) continue;
                unsigned char parms[16] = {0};
                api->process_event(l, fIsVisible, parms);
                on = parms[0] && get<float>(l + oIntensity) > 0;
            }
            static int last = -1;
            if (on != last) {
                last = on;
                MT_LOG("reverse %s (car reverse lamps %d, CurrentGear %d)", on ? "on" : "off", n, get<int8_t>(v + oTrans + tGear));
            }
            return on;
        }
        case Brake:     return get<float>(v + oBrake) > 0.05f;
        case TurnLeft:  return (turn == 1 || turn == 3) && phase;
        case TurnRight: return (turn == 2 || turn == 3) && phase;
        case Hazard:    return turn == 3 && phase;
        case Siren:     return get<int>(v + oCold + cSiren) > 0;
        case EngineOn:  return get<float>(v + oHot + hRpm) > 100;
        case Interior: {
            uintptr_t d = get<uintptr_t>(v + oInteriors); int n = get<int>(v + oInteriors + 8);
            for (int i = 0; i < n && i < 16; i++) { uintptr_t l = get<uintptr_t>(d + i * 8); if (l && get<uint8_t>(l + iOn) & 1) return true; }
            return false;
        }
    }
    return false;
}

static void apply(Fitted& f, DWORD now, uintptr_t driving) {
    // The game's own menus (mode submenu, colour editor) need the vehicle's list --
    // emptied, the UI drops out. Menus only open on foot and the lights only change
    // while driving, so the list is taken away only while the player drives this car.
    detach(f, driving != f.vehicle);
    bool adopted = false;
    for (auto& s : f.sec) {
        if (!alive(s.ctl)) continue;
        uintptr_t c = s.ctl.ptr;
        float cur[4]; rd(c + oColor, cur, 16);
        bool fresh = !near3(cur, s.color);
        for (auto& seen : pending.seen) fresh = fresh && !near3(cur, seen.data());
        if (fresh && pending.kind == 'c' && pending.actor == f.actor.ptr && !adopted) {
            // a colour none of the controls had at click time: the picker's, for the clicked section
            // the picker previews live: keep following it until it is closed, last colour wins
            for (int i : pending.secs) memcpy(f.sec[i].color, cur, 16);
            pending.at = now;  // still in use
            adopted = true;
            save(f);
        }
        if (fabsf(cur[0] - s.color[0]) > 1e-3f || fabsf(cur[1] - s.color[1]) > 1e-3f || fabsf(cur[2] - s.color[2]) > 1e-3f) {
            wr(c + oColor, s.color, 16);
            api->process_event(c, fRepColor, nullptr);
        }
        bool want = s.mode == Manual ? s.on : (f.vehicle && follows(f.vehicle, s.mode, now));
        bool blinks = s.mode == TurnLeft || s.mode == TurnRight || s.mode == Hazard;
        if (s.mode != Manual && !blinks) {  // steady follow modes: ride out a signal that drops for a frame
            if (want) s.lastTrue = now;
            else want = s.lastTrue && now - s.lastTrue < kOffDelay;
        }
        uint8_t b = get<uint8_t>(c + oOn);
        if ((b & 1) != (want ? 1 : 0)) {
            b = (uint8_t)((b & ~1) | (want ? 1 : 0));
            wr(c + oOn, &b, 1);
            api->process_event(c, fRepOn, nullptr);
        }
    }
}

// The attachment whose menu is open: the interaction list's highlighted interactable,
// else any interactable the list holds that belongs to a fitted attachment with this
// section, else the only fitted attachment that has a section of that name.
static Fitted* target(const std::string& section) {
    auto mine = [&](uintptr_t inter) -> Fitted* {
        auto it = inter ? fitted.find(outerOf(inter)) : fitted.end();
        if (it == fitted.end() || it->second.key.empty()) return nullptr;
        return groupOf(it->second, section).empty() ? nullptr : &it->second;
    };
    for (auto& r : lists) {
        if (!alive(r)) continue;
        if (Fitted* f = mine(get<uintptr_t>(r.ptr + oHighlight))) return f;
        uintptr_t d = get<uintptr_t>(r.ptr + oListInters); int n = get<int>(r.ptr + oListInters + 8);
        for (int i = 0; i < n && i < 64; i++) if (Fitted* f = mine(get<uintptr_t>(d + i * 8))) return f;
    }
    Fitted* only = nullptr;
    for (auto& [a, f] : fitted) {
        if (f.key.empty()) continue;
        if (groupOf(f, section).empty()) continue;
        if (only) return nullptr;
        only = &f;
    }
    return only;
}

static void onClick(const std::string& label, DWORD now) {
    if (pending.kind == 'c' && label == "Close") {  // the colour editor's own close button
        auto it = fitted.find(pending.actor);
        if (it != fitted.end()) {
            for (int i : pending.secs) {
                const float* c = it->second.sec[i].color;
                MT_LOG("%s: %s colour %.2f %.2f %.2f", it->second.key.c_str(), it->second.sec[i].name.c_str(), c[0], c[1], c[2]);
            }
        }
        pending = {};
        return;
    }
    if (pending.kind == 'm' && now - pending.at < 30000) {
        int m = modeOf(label);
        auto it = fitted.find(pending.actor);
        if (m >= 0 && it != fitted.end()) {
            for (int i : pending.secs) {
                it->second.sec[i].mode = m;
                MT_LOG("%s: %s mode %s", it->second.key.c_str(), it->second.sec[i].name.c_str(), kModeNames[m]);
            }
            pending = {};
            save(it->second);
            return;
        }
    }
    std::string name;
    char kind = split(label, name);
    if (!kind) return;  // not one of our lines (picker buttons, other menus)
    Fitted* f = target(name);
    MT_LOG("click \"%s\" -> %s", label.c_str(), f ? f->key.c_str() : "no fitted attachment with that section");
    if (!f) return;
    std::vector<int> secs = groupOf(*f, name);
    if (kind == 't') {
        for (int i : secs) {
            // Toggle is the switch of a Manual section; a section following the car
            // (Reverse, Brake, Turn...) keeps following it -- the mode is left alone.
            f->sec[i].on = !f->sec[i].on;
            MT_LOG("%s: %s %s%s", f->key.c_str(), f->sec[i].name.c_str(), f->sec[i].on ? "on" : "off",
                   f->sec[i].mode == Manual ? "" : " (ignored: follows the car)");
        }
        save(*f);
        return;
    }
    pending = {f->actor.ptr, secs, kind, now, {}};
    for (auto& r : f->controls) {  // what the controls show now is not the picker's colour
        float c[4] = {0};
        if (alive(r) && rd(r.ptr + oColor, c, 16)) pending.seen.push_back({c[0], c[1], c[2]});
    }
}

// The pawn the local player controls (the vehicle while driving), or 0.
static uintptr_t localPawn() {
    for (auto& r : controllers)
        if (alive(r) && get<uintptr_t>(r.ptr + oPCPlayer)) return get<uintptr_t>(r.ptr + oPawn);
    return 0;
}

// ---- COPY / PASTE / MIRROR in the placement window ----
static uintptr_t rowSpin(uintptr_t window, int i) {
    uintptr_t row = get<uintptr_t>(window + oPlaceRows[i]);
    return row ? get<uintptr_t>(row + oRowSpin) : 0;
}

static uintptr_t textOf(const wchar_t* text, unsigned char* p) {
    struct { const wchar_t* data; int num, max; } in = {text, (int)wcslen(text) + 1, (int)wcslen(text) + 1};
    memcpy(p + pToIn, &in, sizeof in);
    api->process_event(textCDO, fToText, p);
    return 0;
}

static void labelTool(uintptr_t button, const wchar_t* text) {
    unsigned char p[0x40] = {0};
    textOf(text, p);
    wr(button + oBtnName, p + pToRet, 16);  // W_Button reads it when built, and clicks are matched on it
    uintptr_t block = get<uintptr_t>(button + oBlock), rich = get<uintptr_t>(button + oRich);
    if (block) api->process_event(block, fSetText, p + pToRet);
    if (rich) api->process_event(rich, fSetRich, p + pToRet);
}

// One button under `box`, the game's own size, spaced like its others. A panel has no
// reflected insert, so addTools adds these at the end and puts back what sat below them.
static uintptr_t addTool(uintptr_t window, uintptr_t box, const wchar_t* text) {
    uintptr_t save = get<uintptr_t>(window + oPlaceSave);
    struct { uintptr_t context, cls, player, ret; } c = {window, classOf(save), 0, 0};
    api->process_event(widgetCDO, fCreate, &c);
    if (!c.ret) return 0;
    struct { uintptr_t content, slot; } add = {c.ret, 0};
    api->process_event(box, fAddChild, &add);
    float pad[4] = {0, 4, 0, 4};
    if (add.slot) api->process_event(add.slot, fPadding, pad);
    labelTool(c.ret, text);
    return c.ret;
}

// A section title, the window's own W_Title (as over Location and Rotation).
static void labelTitle(uintptr_t title, const wchar_t* text) {
    unsigned char p[0x40] = {0};
    textOf(text, p);
    if (oTitleText >= 0) wr(title + oTitleText, p + pToRet, 16);  // what it shows when it is built
    uintptr_t block = oTitleBlock >= 0 ? get<uintptr_t>(title + oTitleBlock) : 0;
    // TextBlock_70 is no variable in the shipped widget, and W_Title reads TitleText only while it is
    // built, before this runs. So the text block is found by walking the widget's own tree: its
    // root, then into each container's content or first child, down to the first TextBlock.
    static uintptr_t cUser = api->find_object("Class", "UserWidget", "/Script/UMG"), cTree = api->find_object("Class", "WidgetTree", "/Script/UMG");
    static uintptr_t cText = api->find_object("Class", "TextBlock", "/Script/UMG"), cContent = api->find_object("Class", "ContentWidget", "/Script/UMG");
    static uintptr_t cPanel = api->find_object("Class", "PanelWidget", "/Script/UMG"), fContent = api->find_object("Function", "GetContent", "ContentWidget");
    static int oTree = cUser ? off(cUser, "WidgetTree") : -1, oRoot = cTree ? off(cTree, "RootWidget") : -1;
    if (!block && oTree >= 0 && oRoot >= 0 && cText) {
        uintptr_t tree = get<uintptr_t>(title + oTree), w = tree ? get<uintptr_t>(tree + oRoot) : 0;
        std::string path;
        for (int depth = 0; w && depth < 8 && !api->is_a(classOf(w), cText); depth++) {
            path += objName(classOf(w)) + " > ";
            uintptr_t next = 0;
            if (cContent && fContent && api->is_a(classOf(w), cContent)) { struct { uintptr_t ret; } c = {0}; api->process_event(w, fContent, &c); next = c.ret; }
            else if (cPanel && api->is_a(classOf(w), cPanel)) { struct { int index; uintptr_t ret; } at = {0, 0}; api->process_event(w, fChildAt, &at); next = at.ret; }
            w = next;
        }
        if (w && api->is_a(classOf(w), cText)) block = w;
        static int said;
        if (said++ < 2) MT_LOG("section title text: %s%s", path.c_str(), block ? "TextBlock found" : "no TextBlock");
    }
    if (block) api->process_event(block, fSetText, p + pToRet);
}

static uintptr_t addTitle(uintptr_t window, uintptr_t box, const wchar_t* text) {
    if (!cTitle) return 0;
    struct { uintptr_t context, cls, player, ret; } c = {window, cTitle, 0, 0};
    api->process_event(widgetCDO, fCreate, &c);
    if (!c.ret) return 0;
    struct { uintptr_t content, slot; } add = {c.ret, 0};
    api->process_event(box, fAddChild, &add);
    labelTitle(c.ret, text);
    return c.ret;
}

static Ref refOf(uintptr_t o) { return {o ? get<int>(o + L().obj_index) : 0, o}; }

// The placement window is caught as it is built (its Construct), not on the next 2 s scan: the scan was the delay.
static std::mutex builtLock;
static std::vector<Ref> built;
static int onBuilt(uintptr_t obj, uintptr_t, void*, void*) {
    if (cPlace && obj && api->is_a(classOf(obj), cPlace)) { std::lock_guard<std::mutex> g(builtLock); built.push_back(refOf(obj)); }
    return 0;
}

static void addTools(uintptr_t window) {
    uintptr_t options = get<uintptr_t>(window + oPlaceOptions);
    struct { uintptr_t ret; } parent = {0};
    if (options) api->process_event(options, fGetParent, &parent);
    if (!parent.ret) { MT_LOG("placement window: no place for the buttons"); return; }
    uintptr_t box = parent.ret;
    struct { uintptr_t content; int ret; } where = {options, -1};
    api->process_event(box, fChildIndex, &where);
    int count = 0;
    api->process_event(box, fChildCount, &count);
    // The window's own section titles (Location, Rotation) sit in the same box: W_Title_C. Without
    // one the buttons still go in, untitled.
    for (int i = 0; !cTitle && i < count; i++) {
        struct { int index; uintptr_t ret; } at = {i, 0};
        api->process_event(box, fChildAt, &at);
        if (!at.ret || objName(classOf(at.ret)) != "W_Title_C") continue;
        cTitle = classOf(at.ret);
        oTitleText = off(cTitle, "TitleText");
        oTitleBlock = off(cTitle, "TextBlock_70");
        MT_LOG("section titles: W_Title_C, text +%x, block +%x", oTitleText, oTitleBlock);
    }
    std::vector<uintptr_t> below;
    for (int i = where.ret; where.ret >= 0 && i < count; i++) {
        struct { int index; uintptr_t ret; } at = {i, 0};
        api->process_event(box, fChildAt, &at);
        if (at.ret) below.push_back(at.ret);
    }
    for (uintptr_t w : below) api->process_event(w, fRemove, nullptr);
    titles[0] = refOf(addTitle(window, box, kTitles[0]));
    for (int i = 0; i < 3; i++) tools[i] = refOf(addTool(window, box, kTools[i]));
    titles[1] = refOf(addTitle(window, box, kTitles[1]));
    for (uintptr_t w : below) {
        struct { uintptr_t content, slot; } add = {w, 0};
        api->process_event(box, fAddChild, &add);
    }
    toolsIn = window;
    toolLabels = 60;  // the button and the titles rewrite their text as they are built
}

static void tickPlacement() {
    if (!cPlace || !spinSet) return;
    {
        std::lock_guard<std::mutex> g(builtLock);
        for (auto& r : built) if (std::none_of(places.begin(), places.end(), [&](const Ref& p) { return p.ptr == r.ptr; })) places.push_back(r);
        built.clear();
    }
    placing = {};
    for (auto& r : places)
        if (alive(r) && get<uintptr_t>(r.ptr + oPlaceAttachment) && callBool(r.ptr, fWidgetVisible)) { placing = r; break; }
    if (!placing.ptr) return;
    if (toolsIn != placing.ptr || !alive(tools[0])) addTools(placing.ptr);
    static unsigned beat;
    if (toolLabels > 0 || ++beat % 30 == 0) {  // and now and then after, should anything put the old text back
        if (toolLabels > 0) toolLabels--;
        for (int i = 0; i < 3; i++)
            if (alive(tools[i])) labelTool(tools[i].ptr, kTools[i]);
        for (int i = 0; i < 2; i++)
            if (alive(titles[i])) labelTitle(titles[i].ptr, kTitles[i]);
    }
    if (alive(titles[1])) {  // nothing of its own to set on this part: no Lights title
        int options = 0;
        api->process_event(get<uintptr_t>(placing.ptr + oPlaceOptions), fChildCount, &options);
        uint8_t shown = options > 0 ? 4 : 1;  // ESlateVisibility: SelfHitTestInvisible, Collapsed
        api->process_event(titles[1].ptr, fSetVis, &shown);
    }
}

// The row takes a value only through its arrows, so it is set, nudged up and back down to fire the
// change, and read. On the rotation rows the nudges do not cancel (they turn the part, and the
// rows are read back from it), so whatever it missed by is aimed off and it tries again.
static bool setRow(uintptr_t spin, float v) {
    float aim = v;
    for (int k = 0; k < 6; k++) {
        spinSet(spin, aim);
        api->process_event(spin, k % 2 ? fDown : fUp, nullptr);  // from below on odd tries: Up stops short at the top
        api->process_event(spin, k % 2 ? fUp : fDown, nullptr);
        float now = get<float>(spin + oSpinValue), miss = now - v;
        if (fabs(miss) < 0.05f || fabs(fabs(miss) - 360) < 0.05f) return true;
        aim -= miss;
    }
    return false;
}

static void copyRows() {
    char text[160];
    int n = sprintf_s(text, "AttachmentTweaks");
    for (int i = 0; i < 7; i++) {
        uintptr_t spin = rowSpin(placing.ptr, i);
        n += sprintf_s(text + n, sizeof text - n, " %g", spin ? get<float>(spin + oSpinValue) : 0.0f);
    }
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (size_t)n + 1);
    if (!mem || !OpenClipboard(nullptr)) { if (mem) GlobalFree(mem); MT_LOG("clipboard busy, nothing copied"); return; }
    memcpy(GlobalLock(mem), text, (size_t)n + 1);
    GlobalUnlock(mem);
    EmptyClipboard();
    if (!SetClipboardData(CF_TEXT, mem)) GlobalFree(mem);
    CloseClipboard();
    MT_LOG("copied: %s", text);
}

static void pasteRows(bool mirror) {
    float v[7];
    bool got = false;
    if (OpenClipboard(nullptr)) {
        if (HANDLE mem = GetClipboardData(CF_TEXT)) {
            if (const char* text = (const char*)GlobalLock(mem)) {
                got = sscanf_s(text, "AttachmentTweaks %f %f %f %f %f %f %f", v, v + 1, v + 2, v + 3, v + 4, v + 5, v + 6) == 7
                   || sscanf_s(text, "MoreAttachments %f %f %f %f %f %f %f", v, v + 1, v + 2, v + 3, v + 4, v + 5, v + 6) == 7;  // copied before the rename
                GlobalUnlock(mem);
            }
        }
        CloseClipboard();
    }
    if (!got) { MT_LOG("nothing to paste: the clipboard holds no COPY"); return; }
    if (mirror) { v[1] = -v[1]; v[3] = -v[3]; v[5] = -v[5]; }  // across the car: Y flips, and so do roll and yaw
    uintptr_t scale = get<uintptr_t>(placing.ptr + oPlaceScale);
    // Setting one rotation row can move the others (they are one rotation, read back), so the
    // whole set goes round again until every row reads what was copied.
    for (int pass = 0; pass < 3; pass++) {
        bool all = true;
        for (int i = 0; i < 7; i++) {
            uintptr_t spin = rowSpin(placing.ptr, i);
            if (!spin || (i == 6 && !(scale && callBool(scale, fWidgetVisible)))) continue;
            float now = get<float>(spin + oSpinValue);
            if (fabs(now - v[i]) < 0.05f || fabs(fabs(now - v[i]) - 360) < 0.05f) continue;
            all = false;
            if (!setRow(spin, v[i]) && i >= 3 && i < 6 && v[i] < 0) setRow(spin, v[i] + 360);  // a 0-360 row
        }
        if (all) break;
    }
    for (int i = 0; i < 7; i++) {
        uintptr_t spin = rowSpin(placing.ptr, i);
        if (!spin || (i == 6 && !(scale && callBool(scale, fWidgetVisible)))) continue;
        float now = get<float>(spin + oSpinValue);
        if (fabs(now - v[i]) > 0.05f && fabs(fabs(now - v[i]) - 360) > 0.05f) MT_LOG("%s wanted %g, stopped at %g", kPlaceRows[i], v[i], now);
    }
    MT_LOG(mirror ? "mirrored" : "pasted");
}

static bool toolClick(const std::string& label) {
    if (!placing.ptr) return false;
    if (label == "COPY") copyRows();
    else if (label == "PASTE") pasteRows(false);
    else if (label == "MIRROR") pasteRows(true);
    else return false;
    return true;
}

static void tick(void*) {
    std::lock_guard<std::mutex> g(lock);
    DWORD now = GetTickCount();
    uintptr_t driving = localPawn();
    for (auto& [a, f] : fitted) {
        if (!alive(f.actor)) continue;
        if (!f.parsed) parse(f);
        if (f.sec.empty()) continue;
        if (f.key.empty()) bindVehicle(f);
        if (f.key.empty()) continue;  // not on a vehicle (yet): never touch it
        if (!f.loaded) load(f);
        apply(f, now, driving);
        if (f.dirtyAt && now - f.dirtyAt > 1000) { writeCar(f); f.dirtyAt = 0; }
    }
    if (pending.kind && now - pending.at > 120000) pending = {};
    tickPlacement();

    SHORT ms = GetAsyncKeyState(VK_LBUTTON), es = GetAsyncKeyState(VK_RETURN);
    bool mouse = (ms & 0x8000) != 0, enter = (es & 0x8000) != 0;
    // the low bit: pressed since the last check, so a click shorter than a tick still counts
    bool click = (mouse && !mouseWasDown) || (enter && !enterWasDown) || (!mouse && (ms & 1)) || (!enter && (es & 1));
    mouseWasDown = mouse; enterWasDown = enter;
    if (!click) return;
    for (auto& r : buttons) {
        if (!alive(r)) continue;
        if (!callBool(r.ptr, fHovered) && !callBool(r.ptr, fFocus)) continue;
        std::string label = toStr(r.ptr + oBtnName);
        if (!label.empty()) { if (!toolClick(label)) onClick(label, now); break; }
    }
}

static bool resolve() {
    cCtl = api->find_object("Class", "MTLightControlComponent", "/Script/MotorTown");
    cInter = api->find_object("Class", "MTInteractableComponent", "/Script/MotorTown");
    cBtn = api->find_object("Class", "MTButtonWidget", "/Script/MotorTown");
    cList = api->find_object("Class", "InteractionListWidget", "/Script/MotorTown");
    cVeh = api->find_object("Class", "MTVehicle", "/Script/MotorTown");
    cInterior = api->find_object("Class", "MTVehicleInteriorLight", "/Script/MotorTown");
    cAttComp = api->find_object("Class", "MTVehicleAttachmentPartComponent", "/Script/MotorTown");
    cPC = api->find_object("Class", "PlayerController", "/Script/Engine");
    oPCPlayer = off(cPC, "Player");
    oPawn = off(api->find_object("Class", "Controller", "/Script/Engine"), "Pawn");
    uintptr_t cActor = api->find_object("Class", "Actor", "/Script/Engine");
    uintptr_t sParams = api->find_object("ScriptStruct", "MTInteractionParams", "/Script/MotorTown");
    uintptr_t sCold = api->find_object("ScriptStruct", "MTVehicleColdState", "/Script/MotorTown");
    uintptr_t sTrans = api->find_object("ScriptStruct", "MTNetTransmissionColdState", "/Script/MotorTown");
    uintptr_t sHot = api->find_object("ScriptStruct", "MTNetEngineHotState", "/Script/MotorTown");
    fHovered = api->find_object("Function", "IsHovered", "Widget");
    fFocus = api->find_object("Function", "HasAnyUserFocus", "Widget");
    fToStr = api->find_object("Function", "Conv_TextToString", "KismetTextLibrary");
    textCDO = api->find_object("KismetTextLibrary", "Default__KismetTextLibrary", "/Script/Engine");
    fRepOn = api->find_object("Function", "OnRep_LightOn", "MTLightControlComponent");
    fRepColor = api->find_object("Function", "OnRep_LightColor", "MTLightControlComponent");
    fIsVisible = api->find_object("Function", "IsVisible", "SceneComponent");
    oReverseLights = off(cVeh, "ReverseLights");
    oIntensity = off(api->find_object("Class", "LightComponentBase", "/Script/Engine"), "Intensity");

    oOn = off(cCtl, "Net_bLightOn"); oColor = off(cCtl, "Net_LightColor");
    oParams = off(cInter, "InteractionParams"); pName = off(sParams, "CustomName");
    pSize = sParams ? get<int>(sParams + L().struct_child_props + 8) : 0;  // PropertiesSize follows ChildProperties
    oBtnName = off(cBtn, "ButtonName"); oHighlight = off(cList, "HighlightedInteractable");
    oListInters = off(cList, "InteractableComponents");
    oInText = off(fToStr, "InText"); oRet = off(fToStr, "ReturnValue");
    oVehId = off(cVeh, "Net_VehicleId"); oBrake = off(cVeh, "Brake"); oCold = off(cVeh, "NetLC_ColdState");
    oTrans = off(cVeh, "NetLC_TransmissionColdState"); oHot = off(cVeh, "NetLC_EngineHotState");
    oInteriors = off(cVeh, "InteriorLights");
    cTurn = off(sCold, "TurnSignal"); cHead = off(sCold, "HeadLightMode"); cSiren = off(sCold, "SirenIndex");
    tGear = off(sTrans, "CurrentGear"); hRpm = off(sHot, "CurrentRPM"); iOn = off(cInterior, "bLightOn");
    oOwner = off(cActor, "Owner"); oParentComp = off(cActor, "ParentComponent"); oSlot = off(cAttComp, "Net_PartSlot");
    oCompLights = off(cAttComp, "LightControls");

    cPlace = api->find_object("Class", "AttachmentPlacementWidget", "/Script/MotorTown");
    uintptr_t cRow = api->find_object("Class", "SettingWidget", "/Script/MotorTown");
    widgetCDO = api->find_object("WidgetBlueprintLibrary", "Default__WidgetBlueprintLibrary", "/Script/UMG");
    fCreate = api->find_object("Function", "Create", "WidgetBlueprintLibrary");
    fAddChild = api->find_object("Function", "AddChild", "PanelWidget");
    fGetParent = api->find_object("Function", "GetParent", "Widget");
    fWidgetVisible = api->find_object("Function", "IsVisible", "Widget");
    fToText = api->find_object("Function", "Conv_StringToText", "KismetTextLibrary");
    fSetText = api->find_object("Function", "SetText", "TextBlock");
    fSetRich = api->find_object("Function", "SetText", "RichTextBlock");
    fPadding = api->find_object("Function", "SetPadding", "VerticalBoxSlot");
    fChildIndex = api->find_object("Function", "GetChildIndex", "PanelWidget");
    fChildCount = api->find_object("Function", "GetChildrenCount", "PanelWidget");
    fChildAt = api->find_object("Function", "GetChildAt", "PanelWidget");
    fRemove = api->find_object("Function", "RemoveFromParent", "Widget");
    fSetVis = api->find_object("Function", "SetVisibility", "Widget");
    oPlaceOptions = off(cPlace, "InteractionsVerticalBox");
    fUp = api->find_object("Function", "Up", "SpinBoxString");
    fDown = api->find_object("Function", "Down", "SpinBoxString");
    oPlaceAttachment = off(cPlace, "Attachment"); oPlaceSave = off(cPlace, "SaveButton"); oPlaceScale = off(cPlace, "ScalePanel");
    oRowSpin = off(cRow, "SpinBoxString"); oBlock = off(cBtn, "NameTextBlock"); oRich = off(cBtn, "NameRichTextBlock");
    bool canPlace = cPlace && cRow && widgetCDO && fCreate && fAddChild && fGetParent && fWidgetVisible && fToText && fSetText && fSetRich && fUp && fDown
        && fPadding && fChildIndex && fChildCount && fChildAt && fRemove && fSetVis && oPlaceOptions >= 0
        && oPlaceAttachment >= 0 && oPlaceSave >= 0 && oPlaceScale >= 0 && oRowSpin >= 0 && oBlock >= 0 && oRich >= 0;
    for (int i = 0; i < 7; i++) { oPlaceRows[i] = off(cPlace, kPlaceRows[i]); canPlace = canPlace && oPlaceRows[i] >= 0; }
    if (canPlace) { pToIn = off(fToText, "InString"); pToRet = off(fToText, "ReturnValue"); canPlace = pToIn >= 0 && pToRet >= 0; }
    // Number rows have no reflected setter: the same two signatures ECU Tuning uses.
    auto range = find_sig("48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 0F B6 44 24 50 48 8B D9 88 81 ?? ?? ?? ?? F3 0F 11 89");
    auto value = find_sig("40 53 48 83 EC 20 80 B9 ?? ?? ?? ?? 00 48 8B D9 F3 0F 10 81 ?? ?? ?? ?? F3 0F 5D C1 F3 0F 5F 81");
    if (canPlace && range.size() == 1 && value.size() == 1) {
        spinSet = (decltype(spinSet))value[0];
        oSpinValue = get<int>(range[0] + 57);  // the fourth store: value = min
    } else {
        cPlace = 0;
        MT_LOG("placement window left alone: no COPY/PASTE (%d, %d matches)", (int)range.size(), (int)value.size());
    }
    {  // the car's customisation list, where the settings are kept
        uintptr_t sCustom = api->find_object("ScriptStruct", "MTVehicleCustomization", "/Script/MotorTown");
        uintptr_t sColor = api->find_object("ScriptStruct", "MTVehicleCustomizationColor", "/Script/MotorTown");
        gameplayCDO = api->find_object("GameplayStatics", "Default__GameplayStatics", "/Script/Engine");
        fGetPC = api->find_object("Function", "GetPlayerController", "GameplayStatics");
        fSendCustom = api->find_object("Function", "ServerSetVehicleCustomization", "MotorTownPlayerController");
        stringCDO = api->find_object("KismetStringLibrary", "Default__KismetStringLibrary", "/Script/Engine");
        fToName = api->find_object("Function", "Conv_StringToName", "KismetStringLibrary");
        oCustom = off(cVeh, "Customization");
        if (sCustom && sColor && fSendCustom && fToName) {
            cColors = off(sCustom, "BodyColors"); cSlot = off(sColor, "MaterialSlotName"); cColor = off(sColor, "Color");
            cSize = get<int>(sColor + L().struct_child_props + 8); customSize = get<int>(sCustom + L().struct_child_props + 8);
            pSendVehicle = off(fSendCustom, "Vehicle"); pSendCustom = off(fSendCustom, "Customization");
            pNameIn = off(fToName, "InString"); pNameRet = off(fToName, "ReturnValue");
        }
        if (!sCustom || !sColor || !gameplayCDO || !fGetPC || !fSendCustom || !stringCDO || !fToName || oCustom < 0 || cColors < 0 || cSlot < 0
            || cColor < 0 || cSize <= 0 || pSendVehicle < 0 || pSendCustom < 0 || pNameIn < 0 || pNameRet < 0) {
            MT_LOG("customisation calls not found: light settings are not saved");
            oCustom = -1;
        }
    }
    int need[] = {oOn, oColor, oParams, pName, pSize, oBtnName, oHighlight, oListInters, oInText, oRet, oVehId, oBrake, oCold,
                  oTrans, oHot, oInteriors, cTurn, cHead, cSiren, tGear, hRpm, iOn, oOwner, oParentComp, oSlot, oCompLights, oReverseLights, oIntensity, oPCPlayer, oPawn};
    bool ok = fHovered && fFocus && fToStr && textCDO && fRepOn && fRepColor && fIsVisible;
    for (int v : need) ok = ok && v >= 0;
    MT_LOG("reflection %s | on+%x color+%x params+%x/%x name+%x btn+%x hl+%x veh id+%x brake+%x cold+%x(turn+%x head+%x siren+%x) trans+%x gear+%x hot+%x rpm+%x",
           ok ? "ok" : "INCOMPLETE", oOn, oColor, oParams, pSize, pName, oBtnName, oHighlight, oVehId, oBrake, oCold,
           cTurn, cHead, cSiren, oTrans, tGear, oHot, hRpm);
    return ok;
}

// The tick is queued from a thread of its own: the object scan in run() takes a large part of a second,
// and queuing from the scanning thread stalled the tick for that long each time.
static DWORD WINAPI pump(LPVOID) {
    for (;;) {
        if (api->game_thread_ready()) api->run_on_game_thread(tick, nullptr);
        Sleep(33);
    }
}

static DWORD WINAPI run(LPVOID) {
    MT_LOG("Attachment Tweaks loaded: placement clipboard, per-section toggle / mode / colour for labelled attachments");
    if (!api->wait_engine(INFINITE)) { MT_LOG("FAILED: no engine"); return 0; }
    if (!resolve()) { MT_LOG("FAILED: game layout changed -- plugin idle, lights behave as vanilla"); return 0; }
    static const char* kConstruct[] = {"Construct"};
    if (cPlace) MT_LOG("placement window watch %s", api->watch_functions(kConstruct, 1, onBuilt, nullptr) ? "on" : "OFF -- scans only");
    CloseHandle(CreateThread(nullptr, 0, pump, nullptr, 0, nullptr));
    for (int pass = 0;; pass++) {
        if (pass % 60 == 0) {  // ~2 s: rescan buttons, menus and multi-light attachments
            std::vector<Ref> b, l, pcs, pl;
            std::map<uintptr_t, std::vector<Ref>> ctls;
            std::map<uintptr_t, Ref> inters;
            for (int i = 0, n = api->num_objects(); i < n; i++) {
                uintptr_t o = api->object(i);
                if (!o || isCDO(o)) continue;
                uintptr_t k = classOf(o);
                if (k == cCtl) ctls[outerOf(o)].push_back({i, o});
                else if (k == cInter) inters[outerOf(o)] = {i, o};
                else if (api->is_a(k, cBtn)) b.push_back({i, o});
                else if (api->is_a(k, cList)) l.push_back({i, o});
                else if (api->is_a(k, cPC)) pcs.push_back({i, o});
                else if (cPlace && api->is_a(k, cPlace)) pl.push_back({i, o});
            }
            std::lock_guard<std::mutex> g(lock);
            buttons.swap(b); lists.swap(l); controllers.swap(pcs); places.swap(pl);
            for (auto it = fitted.begin(); it != fitted.end();) it = alive(it->second.actor) ? std::next(it) : fitted.erase(it);
            for (auto& [actor, cs] : ctls) {
                if (cs.size() < 2 || fitted.count(actor) || !inters.count(actor)) continue;
                if (objName(classOf(actor)).find("GeneratedClass") != std::string::npos) continue;  // SCS templates
                std::sort(cs.begin(), cs.end(), [](const Ref& a, const Ref& b) {  // MTLightControl, ...1, ...2
                    return fname(a.ptr + L().obj_name) < fname(b.ptr + L().obj_name);
                });
                int ai = get<int>(actor + L().obj_index);
                Fitted f;
                f.actor = {ai, actor}; f.interactable = inters[actor]; f.controls = cs;
                fitted[actor] = f;
            }
        }
        Sleep(33);
    }
}

extern "C" __declspec(dllexport) int PluginInit(const MtApi* a) {
    // needs only the v1 api (run_on_game_thread, process_event, reflection): any loader will do
    if (!a || a->version < 1 || a->size < offsetof(MtApi, plugin) + sizeof(void*)) return 1;
    api = a;
    // more_attachments.ini stays where it is: it is only read, once a car, to move its settings onto the car.
    MoveFileW((gameDir() + L"attachment_lights.ini").c_str(), (gameDir() + L"more_attachments.ini").c_str());  // older names of it
    MoveFileW((gameDir() + L"moretuning.ini").c_str(), (gameDir() + L"more_attachments.ini").c_str());
    if (iniDisabled(L"attachment_tweaks.ini")) return 0;
    CloseHandle(CreateThread(nullptr, 0, run, nullptr, 0, nullptr));
    return 0;
}
