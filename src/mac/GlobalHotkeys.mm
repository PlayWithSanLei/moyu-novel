#include "mac/GlobalHotkeys.h"

#include <QMetaObject>

#include <unordered_map>

#undef slots
#undef signals
#undef emit
#import <Carbon/Carbon.h>

namespace moyu {
namespace {

struct Binding {
    EventHotKeyRef ref = nullptr;
    GlobalHotkeys* owner = nullptr;
    int role = 0;
};

std::unordered_map<UInt32, Binding> g_bindings;
EventHandlerRef g_handler = nullptr;
UInt32 g_nextId = 1;

UInt32 carbonModifiers(int qtModifiers)
{
    const auto modifiers = static_cast<Qt::KeyboardModifiers>(qtModifiers);
    UInt32 carbon = 0;
    if (modifiers.testFlag(Qt::AltModifier)) {
        carbon |= optionKey;
    }
    if (modifiers.testFlag(Qt::ControlModifier)) {
        carbon |= cmdKey;
    }
    if (modifiers.testFlag(Qt::ShiftModifier)) {
        carbon |= shiftKey;
    }
    if (modifiers.testFlag(Qt::MetaModifier)) {
        carbon |= controlKey;
    }
    return carbon;
}

OSStatus onHotKey(EventHandlerCallRef /*caller*/, EventRef event, void* /*user*/)
{
    EventHotKeyID hotKey{};
    const OSStatus status = GetEventParameter(event,
                                              kEventParamDirectObject,
                                              typeEventHotKeyID,
                                              nullptr,
                                              sizeof(hotKey),
                                              nullptr,
                                              &hotKey);
    if (status != noErr) {
        return status;
    }
    const auto found = g_bindings.find(hotKey.id);
    if (found == g_bindings.end() || found->second.owner == nullptr) {
        return noErr;
    }
    GlobalHotkeys* owner = found->second.owner;
    const int role = found->second.role;
    QMetaObject::invokeMethod(owner, [owner, role] { owner->dispatch(role); }, Qt::QueuedConnection);
    return noErr;
}

void installHandler()
{
    if (g_handler != nullptr) {
        return;
    }
    EventTypeSpec spec{};
    spec.eventClass = kEventClassKeyboard;
    spec.eventKind = kEventHotKeyPressed;
    InstallApplicationEventHandler(onHotKey, 1, &spec, nullptr, &g_handler);
}

void removeHandlerIfIdle()
{
    if (!g_bindings.empty() || g_handler == nullptr) {
        return;
    }
    RemoveEventHandler(g_handler);
    g_handler = nullptr;
}

}  // namespace

GlobalHotkeys::GlobalHotkeys(QObject* parent)
    : QObject(parent)
{
}

GlobalHotkeys::~GlobalHotkeys()
{
    unbindAll();
}

bool GlobalHotkeys::bind(HotkeyRole role, const HotkeyChord& chord)
{
    unbind(role);
    if (!chord.valid() || isReservedSystemHotkey(chord.qtKey, static_cast<Qt::KeyboardModifiers>(chord.qtModifiers))) {
        return false;
    }
    installHandler();
    if (g_handler == nullptr) {
        return false;
    }
    const UInt32 id = g_nextId++;
    EventHotKeyID hotKey{};
    hotKey.signature = 0x4D4F5955;  // 'MOYU'
    hotKey.id = id;
    EventHotKeyRef ref = nullptr;
    const OSStatus status = RegisterEventHotKey(chord.keyCode,
                                                 carbonModifiers(chord.qtModifiers),
                                                 hotKey,
                                                 GetApplicationEventTarget(),
                                                 0,
                                                 &ref);
    if (status != noErr || ref == nullptr) {
        removeHandlerIfIdle();
        return false;
    }
    Binding binding;
    binding.ref = ref;
    binding.owner = this;
    binding.role = static_cast<int>(role);
    g_bindings.emplace(id, binding);
    return true;
}

void GlobalHotkeys::unbind(HotkeyRole role)
{
    for (auto it = g_bindings.begin(); it != g_bindings.end();) {
        if (it->second.owner == this && it->second.role == static_cast<int>(role)) {
            if (it->second.ref != nullptr) {
                UnregisterEventHotKey(it->second.ref);
            }
            it = g_bindings.erase(it);
        } else {
            ++it;
        }
    }
    removeHandlerIfIdle();
}

void GlobalHotkeys::unbindAll()
{
    for (auto it = g_bindings.begin(); it != g_bindings.end();) {
        if (it->second.owner == this) {
            if (it->second.ref != nullptr) {
                UnregisterEventHotKey(it->second.ref);
            }
            it = g_bindings.erase(it);
        } else {
            ++it;
        }
    }
    removeHandlerIfIdle();
}

void GlobalHotkeys::dispatch(int role)
{
    Q_EMIT triggered(role);
}

}  // namespace moyu
