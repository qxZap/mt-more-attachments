// More Attachments -- per-section light control for attachments, as plugins\mt_plugin_MoreAttachments.dll.
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
// Settings persist in Binaries\Win64\more_attachments.ini, keyed by Net_VehicleId and
// the attachment's part slot. An attachment without labelled sections is left alone, and
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
    std::string key;            // ini section, once the vehicle is known
    uintptr_t vehicle = 0, comp = 0;  // comp: the vehicle's MTVehicleAttachmentPartComponent for it
};

// ---- reflection, resolved once ----
static uintptr_t cCtl, cInter, cBtn, cList, cVeh, cInterior, cAttComp, cPC;
static uintptr_t fHovered, fFocus, fToStr, textCDO, fRepOn, fRepColor, fIsVisible;
static int oOn, oColor, oParams, pSize, pName, oBtnName, oInText, oRet, oHighlight, oListInters;
static int oVehId, oBrake, oCold, oTrans, oHot, oInteriors, cTurn, cHead, cSiren, tGear, hRpm, iOn;
static int oOwner, oParentComp, oSlot, oCompLights, oReverseLights, oIntensity, oPCPlayer, oPawn;
static const DWORD kOffDelay = 250;  // ms a follow signal must stay off before the light goes off

static std::mutex lock;
static std::vector<Ref> buttons, lists, controllers;
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

static void save(const Fitted& f) {
    if (f.key.empty()) return;
    std::wstring k(f.key.begin(), f.key.end());
    for (auto& s : f.sec) {
        char v[96];
        sprintf_s(v, "%d %.4f %.4f %.4f %s", s.on ? 1 : 0, s.color[0], s.color[1], s.color[2], kModeNames[s.mode]);
        std::wstring n(s.name.begin(), s.name.end()), vv(v, v + strlen(v));
        // Live state: written through at once, so a crash cannot lose it.
        iniSet(L"more_attachments.ini", k.c_str(), n.c_str(), vv.c_str(), true);
    }
}

static void load(Fitted& f) {
    f.loaded = true;
    std::wstring k(f.key.begin(), f.key.end());
    for (auto& s : f.sec) {
        std::wstring n(s.name.begin(), s.name.end());
        std::wstring b = iniGet(L"more_attachments.ini", k.c_str(), n.c_str());
        if (b.empty()) continue;
        int on = 0; float r, g, bl; wchar_t m[24] = {0};
        if (swscanf_s(b.c_str(), L"%d %f %f %f %23s", &on, &r, &g, &bl, m, 24) >= 4) {
            s.on = on != 0; s.color[0] = r; s.color[1] = g; s.color[2] = bl;
            std::string ms(m, m + wcslen(m));
            for (int i = 0; i <= Hazard; i++) if (ms == kModeNames[i]) s.mode = i;
        }
    }
    MT_LOG("%s: settings %s", f.key.c_str(), f.sec.empty() ? "-" : "loaded");
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
    }
    if (pending.kind && now - pending.at > 120000) pending = {};

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
        if (!label.empty()) { onClick(label, now); break; }
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

    int need[] = {oOn, oColor, oParams, pName, pSize, oBtnName, oHighlight, oListInters, oInText, oRet, oVehId, oBrake, oCold,
                  oTrans, oHot, oInteriors, cTurn, cHead, cSiren, tGear, hRpm, iOn, oOwner, oParentComp, oSlot, oCompLights, oReverseLights, oIntensity, oPCPlayer, oPawn};
    bool ok = fHovered && fFocus && fToStr && textCDO && fRepOn && fRepColor && fIsVisible;
    for (int v : need) ok = ok && v >= 0;
    MT_LOG("reflection %s | on+%x color+%x params+%x/%x name+%x btn+%x hl+%x veh id+%x brake+%x cold+%x(turn+%x head+%x siren+%x) trans+%x gear+%x hot+%x rpm+%x",
           ok ? "ok" : "INCOMPLETE", oOn, oColor, oParams, pSize, pName, oBtnName, oHighlight, oVehId, oBrake, oCold,
           cTurn, cHead, cSiren, oTrans, tGear, oHot, hRpm);
    return ok;
}

static DWORD WINAPI run(LPVOID) {
    MT_LOG("More Attachments loaded: per-section toggle / mode / colour for labelled attachments");
    if (!api->wait_engine(INFINITE)) { MT_LOG("FAILED: no engine"); return 0; }
    if (!resolve()) { MT_LOG("FAILED: game layout changed -- plugin idle, lights behave as vanilla"); return 0; }
    for (int pass = 0;; pass++) {
        if (pass % 30 == 0) {  // ~1 s: rescan buttons, menus and multi-light attachments
            std::vector<Ref> b, l, pcs;
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
            }
            std::lock_guard<std::mutex> g(lock);
            buttons.swap(b); lists.swap(l); controllers.swap(pcs);
            for (auto it = fitted.begin(); it != fitted.end();) it = alive(it->second.actor) ? std::next(it) : fitted.erase(it);
            for (auto& [actor, cs] : ctls) {
                if (cs.size() < 2 || fitted.count(actor) || !inters.count(actor)) continue;
                if (objName(classOf(actor)).find("GeneratedClass") != std::string::npos) continue;  // SCS templates
                std::sort(cs.begin(), cs.end(), [](const Ref& a, const Ref& b) {  // MTLightControl, ...1, ...2
                    return fname(a.ptr + L().obj_name) < fname(b.ptr + L().obj_name);
                });
                int ai = -1;
                for (int i = 0, n = api->num_objects(); i < n && ai < 0; i++) if (api->object(i) == actor) ai = i;
                Fitted f;
                f.actor = {ai, actor}; f.interactable = inters[actor]; f.controls = cs;
                fitted[actor] = f;
            }
        }
        if (api->game_thread_ready()) api->run_on_game_thread(tick, nullptr);
        Sleep(33);
    }
}

extern "C" __declspec(dllexport) int PluginInit(const MtApi* a) {
    // needs only the v1 api (run_on_game_thread, process_event, reflection): any loader will do
    if (!a || a->version < 1 || a->size < offsetof(MtApi, plugin) + sizeof(void*)) return 1;
    api = a;
    MoveFileW((gameDir() + L"attachment_lights.ini").c_str(), (gameDir() + L"more_attachments.ini").c_str());  // settings from before the rename
    MoveFileW((gameDir() + L"moretuning.ini").c_str(), (gameDir() + L"more_attachments.ini").c_str());
    if (iniDisabled(L"more_attachments.ini")) return 0;
    CloseHandle(CreateThread(nullptr, 0, run, nullptr, 0, nullptr));
    return 0;
}
