import ctypes
import datetime
import sys
import xml.etree.ElementTree as ElementTree

UCPTRIE_TYPE_SMALL = 1
UCPTRIE_VALUE_BITS_8 = 2
U_MAX_VERSION_LENGTH = 4
U_MAX_VERSION_STRING_LENGTH = 20

icu = ctypes.cdll.icu

# U_CAPI const char * U_EXPORT2
# u_errorName(UErrorCode code);
icu.u_errorName.restype = ctypes.c_char_p
icu.u_errorName.argtypes = [ctypes.c_int]


def check_error(func: str, error: ctypes.c_int):
    if error.value > 0:
        name = icu.u_errorName(error)
        print(f"{func} failed with {name.decode()} ({error.value})")
        exit(1)


# U_CAPI void U_EXPORT2
# u_getVersion(UVersionInfo versionArray);
icu.u_getVersion.restype = None
icu.u_getVersion.argtypes = [ctypes.c_void_p]
# U_CAPI void U_EXPORT2
# u_versionToString(const UVersionInfo versionArray, char *versionString);
icu.u_versionToString.restype = None
icu.u_versionToString.argtypes = [ctypes.c_void_p, ctypes.c_char_p]


def u_getVersion():
    info = (ctypes.c_uint8 * U_MAX_VERSION_LENGTH)()
    icu.u_getVersion(info)
    str = (ctypes.c_char * U_MAX_VERSION_STRING_LENGTH)()
    icu.u_versionToString(info, str)
    return str.value.decode()


# U_CAPI UMutableCPTrie * U_EXPORT2
# umutablecptrie_open(uint32_t initialValue, uint32_t errorValue, UErrorCode *pErrorCode);
icu.umutablecptrie_open.restype = ctypes.c_void_p
icu.umutablecptrie_open.argtypes = [ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p]


def umutablecptrie_open(initial_value: int) -> ctypes.c_void_p:
    error = ctypes.c_int()
    trie = icu.umutablecptrie_open(initial_value, 0, ctypes.byref(error))
    check_error("umutablecptrie_open", error)
    return trie


# U_CAPI void U_EXPORT2
# umutablecptrie_set(UMutableCPTrie *trie, UChar32 c, uint32_t value, UErrorCode *pErrorCode);
icu.umutablecptrie_set.restype = None
icu.umutablecptrie_set.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p]


def umutablecptrie_set(mutable_trie: ctypes.c_void_p, c: int, value: int):
    error = ctypes.c_int()
    icu.umutablecptrie_set(mutable_trie, c, value, ctypes.byref(error))
    check_error("umutablecptrie_set", error)


# U_CAPI void U_EXPORT2
# umutablecptrie_setRange(UMutableCPTrie *trie, UChar32 start, UChar32 end, uint32_t value, UErrorCode *pErrorCode);
icu.umutablecptrie_setRange.restype = None
icu.umutablecptrie_setRange.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_uint32,
                                        ctypes.c_void_p]


def umutablecptrie_setRange(mutable_trie: ctypes.c_void_p, start: int, end: int, value: int):
    error = ctypes.c_int()
    icu.umutablecptrie_setRange(mutable_trie, start, end, value, ctypes.byref(error))
    check_error("umutablecptrie_set", error)


# U_CAPI UCPTrie * U_EXPORT2
# umutablecptrie_buildImmutable(UMutableCPTrie *trie, UCPTrieType type, UCPTrieValueWidth valueWidth, UErrorCode *pErrorCode);
icu.umutablecptrie_buildImmutable.restype = ctypes.c_void_p
icu.umutablecptrie_buildImmutable.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int, ctypes.c_void_p]


def umutablecptrie_buildImmutable(mutable_trie: ctypes.c_void_p, typ: int, value_width: int) -> ctypes.c_void_p:
    error = ctypes.c_int()
    trie = icu.umutablecptrie_buildImmutable(mutable_trie, typ, value_width, ctypes.byref(error))
    check_error("umutablecptrie_buildImmutable", error)
    return trie


# U_CAPI int32_t U_EXPORT2
# ucptrie_toBinary(const UCPTrie *trie, void *data, int32_t capacity, UErrorCode *pErrorCode);
icu.ucptrie_toBinary.restype = ctypes.c_int32
icu.ucptrie_toBinary.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int32, ctypes.c_void_p]


def ucptrie_toBinary(trie: ctypes.c_void_p) -> [ctypes.c_ubyte]:
    error = ctypes.c_int()
    size = icu.ucptrie_toBinary(trie, ctypes.c_void_p(), 0, ctypes.byref(error))

    data = (ctypes.c_ubyte * size)()
    error = ctypes.c_int()
    icu.ucptrie_toBinary(trie, data, size, ctypes.byref(error))
    check_error("ucptrie_toBinary", error)

    return data


def main():
    if len(sys.argv) != 3 or not sys.argv[1].endswith("ucd.nounihan.grouped.xml"):
        print("main.py <path to ucd.nounihan.grouped.xml> <path to unicode_width_overrides.xml>")
        exit(1)

    east_asian_width = {
        "F": 1,  # full-width
        "W": 1,  # wide
        "A": 2,  # ambiguous
    }

    ns = {"ns": "http://www.unicode.org/ns/2003/ucd/1.0"}
    files = [ElementTree.parse(path).getroot() for path in sys.argv[1:]]
    time = datetime.datetime.utcnow().isoformat(timespec='seconds')
    unicode_version = files[0].find("./ns:description", ns).text
    icu_version = u_getVersion()
    mapping = [-1] * 0x110000

    for root in files:
        for group in root.findall("./ns:repertoire/ns:group", ns):
            group_ea = group.get("ea")
            group_emoji = group.get("Emoji")
            group_epres = group.get("EPres")

            for char in group.findall("ns:char", ns):
                ea = char.get("ea") or group_ea  # east-asian (width)
                emoji = char.get("Emoji") or group_emoji  # emoji
                epres = char.get("EPres") or group_epres  # emoji presentation
                if emoji == "Y" and epres == "Y":
                    value = 1
                else:
                    value = east_asian_width.get(ea, 0)

                cp = char.get("cp")  # codepoint
                if cp is not None:
                    cp = int(cp, 16)
                    cp_first = cp
                    cp_last = cp
                else:
                    cp_first = int(char.get("first-cp"), 16)
                    cp_last = int(char.get("last-cp"), 16)

                for c in range(cp_first, cp_last + 1):
                    mapping[c] = value

    mutable_trie = umutablecptrie_open(0)
    covered = 0

    for cp in range(0, len(mapping)):
        val = mapping[cp]
        if val >= 0:
            covered += 1
        if val > 0:
            umutablecptrie_set(mutable_trie, cp, val)

    trie = umutablecptrie_buildImmutable(mutable_trie, UCPTRIE_TYPE_SMALL, UCPTRIE_VALUE_BITS_8)
    data = ucptrie_toBinary(trie)

    print("// Generated by tools/CodepointWidthDetector/main.py")
    print(f"// on {time}Z from {unicode_version}")
    print(f"// with ICU {icu_version}, UCPTRIE_TYPE_SMALL, UCPTRIE_VALUE_BITS_8")
    print(f"// {covered} codepoints covered")
    print("// clang-format off")
    print("static constexpr uint8_t s_ucpTrieData[] = {", end="")
    for i in range(0, len(data)):
        v = data[i]
        prefix = ""
        if i % 64 == 0 and i != len(data) - 1:
            prefix = "\n    "
        print(f"{prefix}0x{v:02x},", end="")
    print("\n};")
    print("// clang-format on")


if __name__ == '__main__':
    main()
