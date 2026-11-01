/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "EiKeyStateTests.h"

#include "base/EventQueue.h"
#include "deskflow/AppUtil.h"
#include "deskflow/KeyTypes.h"
#include "platform/EiKeyState.h"

#include <QByteArray>
#include <QTemporaryFile>

#include <cstdint>
#include <string>

namespace {
class RecordingEiKeyState : public deskflow::EiKeyState
{
public:
  explicit RecordingEiKeyState(IEventQueue *events) : EiKeyState(nullptr, events)
  {
  }

  int capsPresses = 0;

protected:
  void fakeKey(const Keystroke &key) override
  {
    if (key.m_type != Keystroke::KeyType::Button)
      return;
    const auto &button = key.m_data.m_button;
    if (button.m_button == 58 && button.m_press)
      ++capsPresses;
    updateXkbState(button.m_button + 8, button.m_press);
  }
};

class TestEiKeyState : public deskflow::EiKeyState
{
public:
  using EiKeyState::EiKeyState;
  using EiKeyState::getKeyMap;
};

class TestAppUtil : public AppUtil
{
public:
  int run() override
  {
    return 0;
  }

  void startNode() override
  {
  }

  std::vector<std::string> getKeyboardLayoutList() override
  {
    return {"en"};
  }

  std::string getCurrentLanguageCode() override
  {
    return "en";
  }
};

const char TestKeymap[] = R"XKB(xkb_keymap {
xkb_keycodes "test" {
    minimum = 8;
    maximum = 255;
    <LFSH> = 50;
    <NMLK> = 77;
    <CAPS> = 66;
    <AC01> = 38;
    <AB05> = 56;
    <AB03> = 54;
};
xkb_types "test" {
    type "ONE_LEVEL" {
        modifiers = none;
        level_name[Level1] = "Any";
    };
    type "ALPHABETIC" {
        modifiers = Shift+Lock;
        map[Shift] = Level2;
        map[Lock] = Level2;
        map[Shift+Lock] = Level1;
        level_name[Level1] = "Base";
        level_name[Level2] = "Caps";
    };
};
xkb_compat "test" {
    interpret Shift_L+AnyOfOrNone(all) {
        action = SetMods(modifiers=Shift);
    };
    interpret Num_Lock+AnyOfOrNone(all) {
        action = LockMods(modifiers=Mod2);
    };
    interpret Caps_Lock+AnyOfOrNone(all) {
        action = LockMods(modifiers=Lock);
    };
};
xkb_symbols "test" {
    key <LFSH> { [ Shift_L ] };
    key <NMLK> { [ Num_Lock ] };
    key <CAPS> { [ Caps_Lock ] };
    key <AC01> { type="ALPHABETIC", [ a, A ] };
    key <AB05> { type="ALPHABETIC", [ b, B ] };
    key <AB03> { type="ALPHABETIC", [ c, C ] };
    modifier_map Shift { <LFSH> };
    modifier_map Mod2 { <NMLK> };
    modifier_map Lock { <CAPS> };
};
};)XKB";

// XKB keycodes for TestKeymap.
constexpr std::uint32_t LeftShiftKeycode = 50;
constexpr std::uint32_t NumLockKeycode = 77;
} // namespace

void EiKeyStateTests::initTestCase()
{
  m_arch.init();
  m_log.setFilter(LogLevel::Level::Verbose);
}

void EiKeyStateTests::clearStaleModifiers_shiftDownAndNumLockOn_shiftClearedAndNumLockPreserved()
{
  TestAppUtil appUtil;
  EventQueue eventQueue;
  deskflow::EiKeyState keyState(nullptr, &eventQueue);

  QTemporaryFile keymapFile;
  QVERIFY(keymapFile.open());
  const QByteArray keymapData = QByteArray::fromRawData(TestKeymap, sizeof(TestKeymap) - 1);
  QCOMPARE(keymapFile.write(keymapData), keymapData.size());
  QVERIFY(keymapFile.flush());
  keyState.init(keymapFile.handle(), keymapFile.size());

  keyState.updateXkbState(LeftShiftKeycode, true);
  keyState.updateXkbState(NumLockKeycode, true);
  keyState.updateXkbState(NumLockKeycode, false);

  QVERIFY((keyState.pollActiveModifiers() & KeyModifierShift) != 0);
  QVERIFY((keyState.pollActiveModifiers() & KeyModifierNumLock) != 0);

  keyState.clearStaleModifiers();

  QVERIFY((keyState.pollActiveModifiers() & KeyModifierShift) == 0);
  QVERIFY((keyState.pollActiveModifiers() & KeyModifierNumLock) != 0);
}

void EiKeyStateTests::remoteCapsLock_staysEnabledBetweenLetters()
{
  TestAppUtil appUtil;
  EventQueue eventQueue;
  RecordingEiKeyState keyState(&eventQueue);
  QTemporaryFile file;
  QVERIFY(file.open());
  QCOMPARE(file.write(TestKeymap), qint64(sizeof(TestKeymap) - 1));
  QVERIFY(file.flush());
  keyState.init(file.handle(), file.size());
  keyState.updateKeyMap();
  keyState.updateKeyState();

  for (const char letter : std::string("ABC")) {
    keyState.fakeKeyDown(letter, KeyModifierCapsLock, 1, "en");
    QVERIFY(keyState.fakeKeyRepeat(letter, KeyModifierCapsLock, 1, 1, "en"));
    QVERIFY(keyState.fakeKeyUp(1));
    QVERIFY((keyState.pollActiveModifiers() & KeyModifierCapsLock) != 0);
    QCOMPARE(keyState.capsPresses, 1);
  }

  keyState.fakeKeyDown(kKeyShift_L, KeyModifierCapsLock | KeyModifierShift, 3, "en");
  keyState.fakeKeyDown('a', KeyModifierCapsLock | KeyModifierShift, 1, "en");
  QVERIFY(keyState.fakeKeyUp(1));
  QVERIFY((keyState.pollActiveModifiers() & KeyModifierShift) != 0);
  QVERIFY(keyState.fakeKeyUp(3));
  QVERIFY((keyState.pollActiveModifiers() & KeyModifierShift) == 0);
  QVERIFY((keyState.pollActiveModifiers() & KeyModifierCapsLock) != 0);
  QCOMPARE(keyState.capsPresses, 1);

  // macOS can report lock-off as a key release, which carries no modifier mask.
  // A repeat must also synchronize the next mask while the letter stays held.
  keyState.fakeKeyDown('A', KeyModifierCapsLock, 1, "en");
  QVERIFY(keyState.fakeKeyRepeat('a', 0, 1, 1, "en"));
  QVERIFY(keyState.fakeKeyUp(1));
  QVERIFY((keyState.pollActiveModifiers() & KeyModifierCapsLock) == 0);
  QCOMPARE(keyState.capsPresses, 2);

  for (const char letter : std::string("abc")) {
    keyState.fakeKeyDown(letter, 0, 1, "en");
    QVERIFY(keyState.fakeKeyUp(1));
    QVERIFY((keyState.pollActiveModifiers() & KeyModifierCapsLock) == 0);
    QCOMPARE(keyState.capsPresses, 2);
  }

  keyState.fakeKeyDown('A', KeyModifierShift, 1, "en");
  QVERIFY(keyState.fakeKeyUp(1));
  QVERIFY((keyState.pollActiveModifiers() & KeyModifierCapsLock) == 0);
  QCOMPARE(keyState.capsPresses, 2);
}

void EiKeyStateTests::remoteCapsLock_explicitEventsSynchronizeLock()
{
  TestAppUtil appUtil;
  EventQueue eventQueue;
  RecordingEiKeyState keyState(&eventQueue);
  QTemporaryFile file;
  QVERIFY(file.open());
  QCOMPARE(file.write(TestKeymap), qint64(sizeof(TestKeymap) - 1));
  QVERIFY(file.flush());
  keyState.init(file.handle(), file.size());
  keyState.updateKeyMap();
  keyState.updateKeyState();

  keyState.fakeKeyDown(kKeyCapsLock, KeyModifierCapsLock, 2, "en");
  QVERIFY((keyState.pollActiveModifiers() & KeyModifierCapsLock) != 0);
  QCOMPARE(keyState.capsPresses, 1);
  keyState.fakeKeyDown(kKeyCapsLock, 0, 2, "en");
  QVERIFY((keyState.pollActiveModifiers() & KeyModifierCapsLock) == 0);
  QCOMPARE(keyState.capsPresses, 2);
}

void EiKeyStateTests::getKeyMap_alphabeticModifiers_data()
{
  QTest::addColumn<int>("key");
  QTest::addColumn<int>("modifiers");
  QTest::addColumn<bool>("repeat");
  QTest::addColumn<bool>("explicitShiftCaps");

  for (bool explicitShiftCaps : {false, true}) {
    for (bool repeat : {false, true}) {
      const QByteArray suffix =
          QByteArray(explicitShiftCaps ? "-explicit" : "-implicit") + (repeat ? "-repeat" : "-press");
      QTest::newRow(("lowercase" + suffix).constData()) << int('a') << 0 << repeat << explicitShiftCaps;
      QTest::newRow(("shift" + suffix).constData()) << int('A') << int(KeyModifierShift) << repeat << explicitShiftCaps;
      QTest::newRow(("caps" + suffix).constData())
          << int('A') << int(KeyModifierCapsLock) << repeat << explicitShiftCaps;
      QTest::newRow(("shift-caps" + suffix).constData())
          << int('a') << int(KeyModifierShift | KeyModifierCapsLock) << repeat << explicitShiftCaps;
    }
  }
}

void EiKeyStateTests::getKeyMap_alphabeticModifiers()
{
  QFETCH(int, key);
  QFETCH(int, modifiers);
  QFETCH(bool, repeat);
  QFETCH(bool, explicitShiftCaps);

  TestAppUtil appUtil;
  EventQueue eventQueue;
  TestEiKeyState keyState(nullptr, &eventQueue);
  QTemporaryFile keymapFile;
  QVERIFY(keymapFile.open());
  QByteArray keymapData(TestKeymap);
  if (!explicitShiftCaps) {
    // The standard ALPHABETIC type falls back to Level1 for Shift+Lock.
    keymapData.replace("        map[Shift+Lock] = Level1;\n", "");
  }
  QCOMPARE(keymapFile.write(keymapData), keymapData.size());
  QVERIFY(keymapFile.flush());
  keyState.init(keymapFile.handle(), keymapFile.size());

  deskflow::KeyMap keyMap;
  keyState.getKeyMap(keyMap);
  keyMap.finish();
  deskflow::KeyMap::ModifierToKeys activeModifiers;
  KeyModifierMask state = modifiers;
  deskflow::KeyMap::Keystrokes keys;
  const auto *item = keyMap.mapKey(keys, key, 0, activeModifiers, state, modifiers, repeat, "en");
  QVERIFY(item);
  const bool implicitBase = !explicitShiftCaps && key == 'a';
  QCOMPARE(item->m_required, KeyModifierMask(implicitBase ? 0 : modifiers));
  QCOMPARE(item->m_sensitive, KeyModifierMask(implicitBase ? 0 : KeyModifierShift | KeyModifierCapsLock));
  QCOMPARE(state, KeyModifierMask(modifiers));
  // A correctly mapped letter needs no synthetic Shift or Caps Lock events.
  if (repeat) {
    // Linux lets the compositor repeat held keys; no additional events are needed.
    QVERIFY(keys.empty());
    return;
  }
  QCOMPARE(keys.size(), size_t(1));
  QCOMPARE(keys.front().m_type, deskflow::KeyMap::Keystroke::KeyType::Button);
  QCOMPARE(keys.front().m_data.m_button.m_button, KeyButton(38 - 8));
  QVERIFY(keys.front().m_data.m_button.m_press);
  QCOMPARE(keys.front().m_data.m_button.m_repeat, repeat);
}

QTEST_MAIN(EiKeyStateTests)
