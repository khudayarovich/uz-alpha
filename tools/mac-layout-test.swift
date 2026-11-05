// Types test words through the UzAlpha keyboard layouts using macOS's own
// keyboard translation (UCKeyTranslate), so the layout files are checked by the
// system that will run them. Runs on a Mac or a macOS CI runner:
//
//   swift tools/mac-layout-test.swift mac/UzAlpha.bundle
//
// The bundle is copied to ~/Library/Keyboard Layouts and registered; nothing is
// selected or enabled.
import Carbon
import Foundation

guard CommandLine.arguments.count == 2 else {
    print("usage: swift tools/mac-layout-test.swift <UzAlpha.bundle>")
    exit(2)
}

let source = URL(fileURLWithPath: CommandLine.arguments[1]).standardizedFileURL
let fm = FileManager.default
let folder = fm.homeDirectoryForCurrentUser.appendingPathComponent("Library/Keyboard Layouts")
let installed = folder.appendingPathComponent("UzAlpha.bundle")
do {
    try fm.createDirectory(at: folder, withIntermediateDirectories: true)
    if fm.fileExists(atPath: installed.path) { try fm.removeItem(at: installed) }
    // Copy next to the destination and rename, so the system never sees a
    // half-copied bundle.
    let staging = folder.appendingPathComponent(".UzAlpha.staging")
    if fm.fileExists(atPath: staging.path) { try fm.removeItem(at: staging) }
    try fm.copyItem(at: source, to: staging)
    try fm.moveItem(at: staging, to: installed)
} catch {
    print("cannot copy the bundle: \(error)")
    exit(2)
}
print("TISRegisterInputSource: \(TISRegisterInputSource(installed as CFURL))")

func stringProperty(_ input: TISInputSource, _ key: CFString) -> String? {
    guard let raw = TISGetInputSourceProperty(input, key) else { return nil }
    return Unmanaged<CFString>.fromOpaque(raw).takeUnretainedValue() as String
}

func languages(_ input: TISInputSource) -> [String] {
    guard let raw = TISGetInputSourceProperty(input, kTISPropertyInputSourceLanguages) else { return [] }
    return (Unmanaged<CFArray>.fromOpaque(raw).takeUnretainedValue() as NSArray).compactMap { $0 as? String }
}

var idProblems = 0

// Finds a layout of ours by the name macOS shows for it, and checks the input
// source id and language macOS gave it against the bundle's Info.plist.
func layoutData(named name: String, expectedID: String) -> Data? {
    guard let all = TISCreateInputSourceList(nil, true)?.takeRetainedValue() as NSArray? else { return nil }
    var result: Data?
    for item in all {
        let input = item as! TISInputSource
        guard let id = stringProperty(input, kTISPropertyInputSourceID), id.lowercased().contains("uzalpha"),
              let shown = stringProperty(input, kTISPropertyLocalizedName),
              shown.precomposedStringWithCanonicalMapping == name.precomposedStringWithCanonicalMapping,
              let raw = TISGetInputSourceProperty(input, kTISPropertyUnicodeKeyLayoutData)
        else { continue }
        let langs = languages(input)
        print("registered \"\(shown)\": id \(id), first language \(langs.first ?? "none")")
        // macOS has been seen to keep its own generated id for one layout while
        // taking the Info.plist id for the other; either is fine as long as it
        // belongs to our bundle. The language decides where the keyboard is
        // listed, so that one must be right.
        if id != expectedID { print("  (Info.plist asks for \(expectedID))") }
        if !id.hasPrefix("uz.uzalpha.keyboardlayout.uzalpha") {
            idProblems += 1
            print("  FAIL: the id does not belong to the UzAlpha bundle")
        }
        if langs.first != "uz" {
            idProblems += 1
            print("  FAIL: Uzbek should be the first language")
        }
        if result == nil { result = Unmanaged<CFData>.fromOpaque(raw).takeUnretainedValue() as Data }
    }
    return result
}

// US (ANSI) key codes of the characters used below.
let keyCodes: [Character: UInt16] = [
    "a": 0, "s": 1, "d": 2, "f": 3, "h": 4, "g": 5, "z": 6, "x": 7, "c": 8, "v": 9, "b": 11,
    "q": 12, "w": 13, "e": 14, "r": 15, "y": 16, "t": 17, "o": 31, "u": 32, "i": 34, "p": 35,
    "l": 37, "j": 38, "k": 40, "n": 45, "m": 46,
    "'": 39, ";": 41, "[": 33, "]": 30, " ": 49, "`": 50,
]

func type(_ data: Data, _ text: String) -> String {
    var out = ""
    data.withUnsafeBytes { (raw: UnsafeRawBufferPointer) in
        let layout = raw.baseAddress!.assumingMemoryBound(to: UCKeyboardLayout.self)
        var deadKeyState: UInt32 = 0

        func press(_ code: UInt16, shift: Bool) {
            var chars = [UniChar](repeating: 0, count: 8)
            var length = 0
            let modifiers: UInt32 = shift ? UInt32((shiftKey >> 8) & 0xFF) : 0
            let status = UCKeyTranslate(layout, code, UInt16(kUCKeyActionDown), modifiers,
                                        UInt32(LMGetKbdType()), 0, &deadKeyState, chars.count, &length, &chars)
            if status != noErr {
                print("UCKeyTranslate failed: \(status)")
                exit(1)
            }
            out += String(utf16CodeUnits: chars, count: length)
        }

        for ch in text {
            let lower = Character(String(ch).lowercased())
            guard let code = keyCodes[lower] else {
                print("no key for \(ch)")
                exit(2)
            }
            press(code, shift: String(ch) != String(ch).lowercased())
        }
        // A letter still pending at the end is shown as marked text on screen;
        // a space commits it. Drop that space again.
        if deadKeyState != 0 {
            press(49, shift: false)
            if out.hasSuffix(" ") { out.removeLast() }
        }
    }
    return out
}

var failed = 0
var checks = 0

func expect(_ data: Data, _ name: String, _ input: String, _ want: String) {
    checks += 1
    let got = type(data, input)
    if got == want {
        print("ok    \(name): \"\(input)\" -> \"\(got)\"")
    } else {
        failed += 1
        print("FAIL  \(name): \"\(input)\" -> \"\(got)\" (want \"\(want)\")")
    }
}

guard let auto = layoutData(named: "Özbekça (lotin, avtomatik)", expectedID: "uz.uzalpha.keyboardlayout.uzalpha.auto"),
      let manual = layoutData(named: "Özbekça (lotin, qölda)", expectedID: "uz.uzalpha.keyboardlayout.uzalpha.manual")
else {
    print("macOS did not accept the layouts. Input sources it knows that mention uzalpha:")
    if let all = TISCreateInputSourceList(nil, true)?.takeRetainedValue() as NSArray? {
        for item in all {
            let input = item as! TISInputSource
            if let id = stringProperty(input, kTISPropertyInputSourceID), id.lowercased().contains("uzalpha") {
                print("  \(id)  \"\(stringProperty(input, kTISPropertyLocalizedName) ?? "?")\"")
            }
        }
    }
    exit(1)
}

expect(auto, "auto", "shahar", "şahar")
expect(auto, "auto", "choy", "çoy")
expect(auto, "auto", "o'zbek", "özbek")
expect(auto, "auto", "g'isht", "ğişt")
expect(auto, "auto", "o`zbek to`g`ri", "özbek töğri")
expect(auto, "auto", "tog'", "toğ")
expect(auto, "auto", "to'g'ri", "töğri")
expect(auto, "auto", "mashhur", "maşhur")
expect(auto, "auto", "Shahar", "Şahar")
expect(auto, "auto", "SHAHAR", "ŞAHAR")
expect(auto, "auto", "O'zbek", "Özbek")
expect(auto, "auto", "ss", "ss")
expect(auto, "auto", "bos", "bos")
expect(auto, "auto", "ma'no", "maʼno")
expect(auto, "auto", "is'hoq", "isʼhoq")
expect(auto, "auto", "salom; [dunyo]", "salom; [dunyo]")
expect(auto, "auto", "a`b", "a`b")
expect(auto, "auto", "Yangi o'zbek alifbosi uchun", "Yangi özbek alifbosi uçun")
expect(manual, "manual", ";'[]", "şçöğ")
expect(manual, "manual", "salom", "salom")

print("\(checks) typing checks, \(failed) failed; \(idProblems) registration problem(s)")
exit(failed == 0 && idProblems == 0 ? 0 : 1)
