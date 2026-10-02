// SPDX-License-Identifier: MPL-2.0

// Package sigil derives the platform-native title ID from a ROM file and
// resolves the save unit an emulator keeps for it.
package sigil

/*
#cgo CFLAGS: -I${SRCDIR}/../../include
#cgo LDFLAGS: -L${SRCDIR}/../../build -lsigil -lsigil_chdr -lsigil_zstd -lsigil_zlib -lsigil_lzma -lsigil_aes

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#endif
#include "sigil.h"

static sigil_io *sigil_go_open_member(void *ctx, const char *relative_path) {
    char path[SIGIL_SAVE_PATH_MAX * 2];
    int n = snprintf(path, sizeof(path), "%s/%s", (const char *)ctx, relative_path);
    if (n <= 0 || (size_t)n >= sizeof(path)) return NULL;
    return sigil_io_open_file(path);
}

static void sigil_go_set_open(sigil_save_request *req, char *root) {
    req->open = root ? sigil_go_open_member : NULL;
    req->open_ctx = root;
}

static int sigil_go_hash(sigil_save_unit *unit, char *root) {
    return sigil_save_hash(unit, sigil_go_open_member, root);
}

static int sigil_go_make_parents(char *path) {
    for (char *p = path + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
#ifdef _WIN32
        int rc = _mkdir(path);
#else
        int rc = mkdir(path, 0755);
#endif
        *p = '/';
        if (rc != 0 && errno != EEXIST) return -1;
    }
    return 0;
}

static int sigil_go_write_member(void *ctx, const char *relative_path, const uint8_t *data, size_t len) {
    char path[SIGIL_SAVE_PATH_MAX * 2];
    int n = snprintf(path, sizeof(path), "%s/%s", (const char *)ctx, relative_path);
    if (n <= 0 || (size_t)n >= sizeof(path) || sigil_go_make_parents(path) != 0) return -1;
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    size_t wrote = fwrite(data, 1, len, f);
    int closed = fclose(f);
    return wrote == len && closed == 0 ? 0 : -1;
}

static int sigil_go_remove_member(void *ctx, const char *relative_path) {
    char path[SIGIL_SAVE_PATH_MAX * 2];
    int n = snprintf(path, sizeof(path), "%s/%s", (const char *)ctx, relative_path);
    if (n <= 0 || (size_t)n >= sizeof(path)) return -1;
    return remove(path) == 0 ? 0 : -1;
}

static void sigil_go_set_sync_io(sigil_sync_request *req, char *root) {
    req->save.open = sigil_go_open_member;
    req->save.open_ctx = root;
    req->write = sigil_go_write_member;
    req->remove = sigil_go_remove_member;
    req->write_ctx = root;
}
*/
import "C"

import (
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"sort"
	"unsafe"
)

// Platform identifies a console platform.
type Platform int

const (
	PlatformAuto      Platform = C.SIGIL_PLATFORM_AUTO
	PlatformPSP       Platform = C.SIGIL_PLATFORM_PSP
	PlatformPSX       Platform = C.SIGIL_PLATFORM_PSX
	PlatformPS2       Platform = C.SIGIL_PLATFORM_PS2
	PlatformPSVita    Platform = C.SIGIL_PLATFORM_PSVITA
	PlatformSwitch    Platform = C.SIGIL_PLATFORM_SWITCH
	Platform3DS       Platform = C.SIGIL_PLATFORM_3DS
	PlatformWii       Platform = C.SIGIL_PLATFORM_WII
	PlatformWiiU      Platform = C.SIGIL_PLATFORM_WIIU
	PlatformGameCube  Platform = C.SIGIL_PLATFORM_GAMECUBE
	PlatformPS3       Platform = C.SIGIL_PLATFORM_PS3
	PlatformXbox360   Platform = C.SIGIL_PLATFORM_XBOX360
	PlatformDreamcast Platform = C.SIGIL_PLATFORM_DREAMCAST
	PlatformXbox      Platform = C.SIGIL_PLATFORM_XBOX
	PlatformGB        Platform = C.SIGIL_PLATFORM_GB
	PlatformGBC       Platform = C.SIGIL_PLATFORM_GBC
	PlatformSNES      Platform = C.SIGIL_PLATFORM_SNES
)

// FeatureRTC marks a cart with a real-time clock; a libretro frontend
// persists it beside the save as <stem>.rtc.
const FeatureRTC uint32 = C.SIGIL_FEATURE_RTC

// Slug returns the canonical string slug for this platform.
func (p Platform) Slug() string {
	return C.GoString(C.sigil_platform_to_slug(C.sigil_platform(p)))
}

func (p Platform) String() string { return p.Slug() }

// PlatformFromSlug parses a slug; unknown slugs return PlatformAuto.
func PlatformFromSlug(slug string) Platform {
	cs := C.CString(slug)
	defer C.free(unsafe.Pointer(cs))
	return Platform(C.sigil_platform_from_slug(cs))
}

// Source distinguishes binary-extracted IDs from filename-pattern matches.
type Source int

const (
	SourceBinary   Source = C.SIGIL_SOURCE_BINARY
	SourceFilename Source = C.SIGIL_SOURCE_FILENAME
)

func (s Source) String() string {
	switch s {
	case SourceBinary:
		return "binary"
	case SourceFilename:
		return "filename"
	default:
		return fmt.Sprintf("Source(%d)", int(s))
	}
}

// Usage classifies how the platform uses the save ID for save artifacts;
// see README, "usage".
type Usage int

const (
	UsageFolderExact  Usage = C.SIGIL_USAGE_FOLDER_EXACT
	UsageFolderPrefix Usage = C.SIGIL_USAGE_FOLDER_PREFIX
	UsageFileExact    Usage = C.SIGIL_USAGE_FILE_EXACT
	UsageFilePrefix   Usage = C.SIGIL_USAGE_FILE_PREFIX
	UsageFolderSplit  Usage = C.SIGIL_USAGE_FOLDER_SPLIT
)

func (u Usage) String() string {
	switch u {
	case UsageFolderExact:
		return "folder-exact"
	case UsageFolderPrefix:
		return "folder-prefix"
	case UsageFileExact:
		return "file-exact"
	case UsageFilePrefix:
		return "file-prefix"
	case UsageFolderSplit:
		return "folder-split"
	default:
		return fmt.Sprintf("Usage(%d)", int(u))
	}
}

// SwitchContentType is the CNMT content-meta type of a Switch dump.
type SwitchContentType int

const (
	SwitchContentUnknown     SwitchContentType = C.SIGIL_SWITCH_CONTENT_UNKNOWN
	SwitchContentApplication SwitchContentType = C.SIGIL_SWITCH_CONTENT_APPLICATION
	SwitchContentPatch       SwitchContentType = C.SIGIL_SWITCH_CONTENT_PATCH
	SwitchContentAddon       SwitchContentType = C.SIGIL_SWITCH_CONTENT_ADDON
)

func (c SwitchContentType) String() string {
	switch c {
	case SwitchContentApplication:
		return "application"
	case SwitchContentPatch:
		return "patch"
	case SwitchContentAddon:
		return "addon"
	default:
		return "unknown"
	}
}

// Result is a successful extraction.
type Result struct {
	TitleID           string
	RawSerial         string
	SaveID            string
	Platform          Platform
	PlatformSlug      string
	Source            Source
	Usage             Usage
	Experimental      bool
	SwitchContentType SwitchContentType
	TitleVersion      uint32
	Features          uint32
}

// HasRTC reports whether the cart carries a real-time clock.
func (r *Result) HasRTC() bool { return r.Features&FeatureRTC != 0 }

// PersistedResult rebuilds a result from stored columns, or builds one for
// a platform that has no title id. platformSlug selects the save layout.
func PersistedResult(platformSlug, titleID, saveID string, features uint32) *Result {
	return &Result{
		TitleID:      titleID,
		SaveID:       saveID,
		Platform:     PlatformFromSlug(platformSlug),
		PlatformSlug: platformSlug,
		Features:     features,
	}
}

// Options controls extraction behavior. Zero value is valid.
type Options struct {
	SwitchHeaderKey         []byte // 32 bytes; wins over path/blob if set
	SwitchProdKeysPath      string
	SwitchProdKeysBlob      []byte
	DisableFilenameFallback bool
	Allow3DSHomebrew        bool
}

var (
	ErrInvalidArg        = errors.New("sigil: invalid argument")
	ErrIO                = errors.New("sigil: I/O error")
	ErrUnknownPlatform   = errors.New("sigil: unknown platform")
	ErrUnsupportedFormat = errors.New("sigil: unsupported format")
	ErrNotFound          = errors.New("sigil: title id not found")
	ErrNeedsKey          = errors.New("sigil: decryption key required")
	ErrCrypto            = errors.New("sigil: crypto failure")
	ErrOOM               = errors.New("sigil: out of memory")
	ErrConflict          = errors.New("sigil: the saves changed locally since the last sync")
	ErrExists            = errors.New("sigil: a save with that name already exists")
	ErrNoSpace           = errors.New("sigil: not enough free space")
	ErrUncollected       = errors.New("sigil: the volume holds saves not collected yet")
)

func errFromCode(rc C.int) error {
	switch rc {
	case C.SIGIL_OK:
		return nil
	case C.SIGIL_ERR_INVALID_ARG:
		return ErrInvalidArg
	case C.SIGIL_ERR_IO:
		return ErrIO
	case C.SIGIL_ERR_UNKNOWN_PLATFORM:
		return ErrUnknownPlatform
	case C.SIGIL_ERR_UNSUPPORTED_FORMAT:
		return ErrUnsupportedFormat
	case C.SIGIL_ERR_NOT_FOUND:
		return ErrNotFound
	case C.SIGIL_ERR_NEEDS_KEY:
		return ErrNeedsKey
	case C.SIGIL_ERR_CRYPTO:
		return ErrCrypto
	case C.SIGIL_ERR_OOM:
		return ErrOOM
	case C.SIGIL_ERR_CONFLICT:
		return ErrConflict
	case C.SIGIL_ERR_EXISTS:
		return ErrExists
	case C.SIGIL_ERR_NO_SPACE:
		return ErrNoSpace
	case C.SIGIL_ERR_UNCOLLECTED:
		return ErrUncollected
	default:
		return fmt.Errorf("sigil: error %d", int(rc))
	}
}

// LoadHeaderKeyFromProdKeys reads the 32-byte Switch header key from a
// prod.keys file.
func LoadHeaderKeyFromProdKeys(path string) ([]byte, error) {
	cpath := C.CString(path)
	defer C.free(unsafe.Pointer(cpath))
	out := (*C.uint8_t)(C.calloc(32, 1))
	defer C.free(unsafe.Pointer(out))
	if err := errFromCode(C.sigil_load_header_key_from_prod_keys(cpath, out)); err != nil {
		return nil, err
	}
	return C.GoBytes(unsafe.Pointer(out), 32), nil
}

// Extract reads the title ID from path. Pass PlatformAuto to sniff from
// the file extension. opts may be nil. docs/go.md defines every input.
func Extract(path string, platform Platform, opts *Options) (*Result, error) {
	cpath := C.CString(path)
	defer C.free(unsafe.Pointer(cpath))

	// Allocate options + support on the C heap. cgo forbids passing a Go
	// struct that contains a Go pointer to another Go-allocated struct.
	coptions := (*C.sigil_options)(C.calloc(1, C.sizeof_sigil_options))
	defer C.free(unsafe.Pointer(coptions))
	coptions.struct_version = C.SIGIL_OPTIONS_V1

	var (
		csupport  *C.sigil_support
		ckeyBytes unsafe.Pointer
		ckeysPath *C.char
		ckeysBlob unsafe.Pointer
	)
	defer func() {
		if csupport != nil {
			C.free(unsafe.Pointer(csupport))
		}
		if ckeyBytes != nil {
			C.free(ckeyBytes)
		}
		if ckeysPath != nil {
			C.free(unsafe.Pointer(ckeysPath))
		}
		if ckeysBlob != nil {
			C.free(ckeysBlob)
		}
	}()

	if opts != nil {
		if !opts.DisableFilenameFallback {
			coptions.flags |= C.SIGIL_FLAG_FILENAME_FALLBACK
		}
		if opts.Allow3DSHomebrew {
			coptions.flags |= C.SIGIL_FLAG_3DS_ALLOW_HOMEBREW
		}

		needSupport := len(opts.SwitchHeaderKey) > 0 ||
			opts.SwitchProdKeysPath != "" ||
			len(opts.SwitchProdKeysBlob) > 0
		if needSupport {
			csupport = (*C.sigil_support)(C.calloc(1, C.sizeof_sigil_support))
			csupport.struct_version = C.SIGIL_SUPPORT_V1

			if len(opts.SwitchHeaderKey) > 0 {
				if len(opts.SwitchHeaderKey) != 32 {
					return nil, fmt.Errorf("sigil: SwitchHeaderKey must be 32 bytes, got %d", len(opts.SwitchHeaderKey))
				}
				ckeyBytes = C.CBytes(opts.SwitchHeaderKey)
				csupport.switch_header_key = (*C.uchar)(ckeyBytes)
			}
			if opts.SwitchProdKeysPath != "" {
				ckeysPath = C.CString(opts.SwitchProdKeysPath)
				csupport.switch_prod_keys_path = ckeysPath
			}
			if len(opts.SwitchProdKeysBlob) > 0 {
				ckeysBlob = C.CBytes(opts.SwitchProdKeysBlob)
				csupport.switch_prod_keys_text = (*C.char)(ckeysBlob)
				csupport.switch_prod_keys_text_len = C.size_t(len(opts.SwitchProdKeysBlob))
			}
			coptions.support = csupport
		}
	} else {
		coptions.flags = C.SIGIL_FLAG_FILENAME_FALLBACK
	}

	var cresult C.sigil_result
	cresult.struct_version = C.SIGIL_RESULT_V3
	rc := C.sigil_extract_from_path(cpath, C.sigil_platform(platform), coptions, &cresult)
	if err := errFromCode(rc); err != nil {
		return nil, err
	}

	return &Result{
		TitleID:           C.GoString(&cresult.title_id[0]),
		RawSerial:         C.GoString(&cresult.raw_serial[0]),
		SaveID:            C.GoString(&cresult.save_id[0]),
		Platform:          Platform(cresult.platform),
		PlatformSlug:      Platform(cresult.platform).Slug(),
		Source:            Source(cresult.source),
		Usage:             Usage(cresult.usage),
		Experimental:      cresult.experimental != 0,
		SwitchContentType: SwitchContentType(cresult.switch_content_type),
		TitleVersion:      uint32(cresult.title_version),
		Features:          uint32(cresult.features),
	}, nil
}

// SaveShape is the archive shape a save unit travels in; see README, "Save units".
type SaveShape int

const (
	SaveShapeNone   SaveShape = C.SIGIL_SAVE_SHAPE_NONE
	SaveShapeSingle SaveShape = C.SIGIL_SAVE_SHAPE_SINGLE
	SaveShapeMulti  SaveShape = C.SIGIL_SAVE_SHAPE_MULTI
	SaveShapeFolder SaveShape = C.SIGIL_SAVE_SHAPE_FOLDER
)

func (s SaveShape) String() string {
	switch s {
	case SaveShapeNone:
		return "none"
	case SaveShapeSingle:
		return "single"
	case SaveShapeMulti:
		return "multi"
	case SaveShapeFolder:
		return "folder"
	default:
		return fmt.Sprintf("SaveShape(%d)", int(s))
	}
}

// SaveRole is what a member is to its unit.
type SaveRole int

const (
	SaveRolePrimary SaveRole = C.SIGIL_SAVE_ROLE_PRIMARY
	SaveRoleSidecar SaveRole = C.SIGIL_SAVE_ROLE_SIDECAR
	SaveRoleRTC     SaveRole = C.SIGIL_SAVE_ROLE_RTC
)

func (r SaveRole) String() string {
	switch r {
	case SaveRolePrimary:
		return "primary"
	case SaveRoleSidecar:
		return "sidecar"
	case SaveRoleRTC:
		return "rtc"
	default:
		return fmt.Sprintf("SaveRole(%d)", int(r))
	}
}

// SaveMember is one file of a save unit. Path is relative to the save root;
// Entry is its archive name.
type SaveMember struct {
	Path    string
	Entry   string
	Role    SaveRole
	Present bool
}

// SaveUnit is every file under a save root that belongs to one game, and the
// hashes the RomM server computes for it.
type SaveUnit struct {
	Key          string
	Shape        SaveShape
	Members      []SaveMember
	Expected     []SaveMember
	Unkeyed      []string
	Artifact     string
	ContentHash  string
	IdentityHash string
}

// LocateOptions are the optional inputs to LocateSaves. SaveRoot has the
// root listed; Listing (root-relative paths) comes from your own filesystem
// layer instead. Options are the core's current option values; only the
// keys the layout names are read.
type LocateOptions struct {
	SaveRoot string
	Listing  []string
	Options  map[string]string
}

// ContentStem is the base name RetroArch names save files after; see
// README, "Save units".
func ContentStem(contentPath string) string {
	cname := C.CString(contentPath)
	defer C.free(unsafe.Pointer(cname))
	out := (*C.char)(C.calloc(C.SIGIL_SAVE_ENTRY_MAX, 1))
	defer C.free(unsafe.Pointer(out))
	C.sigil_content_stem(cname, out, C.SIGIL_SAVE_ENTRY_MAX)
	return C.GoString(out)
}

// LayoutSubdirs names the subfolders under the save root a layout writes
// into, so the caller knows what to list.
func LayoutSubdirs(layout string) []string {
	const cap = 16
	clayout := C.CString(layout)
	defer C.free(unsafe.Pointer(clayout))
	out := (**C.char)(C.calloc(cap, C.size_t(unsafe.Sizeof((*C.char)(nil)))))
	defer C.free(unsafe.Pointer(out))
	n := int(C.sigil_save_layout_subdirs(clayout, out, cap))
	subdirs := make([]string, 0, n)
	for _, s := range unsafe.Slice(out, n) {
		subdirs = append(subdirs, C.GoString(s))
	}
	return subdirs
}

const subdirListDepth = 4

// ListSaveRoot returns the root-relative paths of the files directly in root
// plus those under the layout's subfolders.
func ListSaveRoot(root, layout string) ([]string, error) {
	var out []string
	entries, err := os.ReadDir(root)
	if err != nil && !errors.Is(err, os.ErrNotExist) {
		return nil, err
	}
	for _, e := range entries {
		if e.Type().IsRegular() {
			out = append(out, e.Name())
		}
	}
	for _, subdir := range LayoutSubdirs(layout) {
		if err := listRecursive(filepath.Join(root, subdir), subdir, subdirListDepth, &out); err != nil {
			return nil, err
		}
	}
	return out, nil
}

func listRecursive(dir, relative string, depth int, out *[]string) error {
	if depth == 0 {
		return nil
	}
	entries, err := os.ReadDir(dir)
	if err != nil {
		if errors.Is(err, os.ErrNotExist) {
			return nil
		}
		return err
	}
	for _, e := range entries {
		rel := relative + "/" + e.Name()
		if e.IsDir() {
			if err := listRecursive(filepath.Join(dir, e.Name()), rel, depth-1, out); err != nil {
				return err
			}
		} else if e.Type().IsRegular() {
			*out = append(*out, rel)
		}
	}
	return nil
}

// cAllocs tracks C memory a call hands to sigil, freed together afterwards.
type cAllocs struct {
	ptrs []unsafe.Pointer
}

func (a *cAllocs) alloc(size C.size_t) unsafe.Pointer {
	p := C.calloc(1, size)
	a.ptrs = append(a.ptrs, p)
	return p
}

func (a *cAllocs) str(s string) *C.char {
	p := C.CString(s)
	a.ptrs = append(a.ptrs, unsafe.Pointer(p))
	return p
}

func (a *cAllocs) strings(items []string) **C.char {
	arr := (**C.char)(a.alloc(C.size_t(len(items)+1) * C.size_t(unsafe.Sizeof((*C.char)(nil)))))
	for i, s := range items {
		unsafe.Slice(arr, len(items))[i] = a.str(s)
	}
	return arr
}

func (a *cAllocs) free() {
	for _, p := range a.ptrs {
		C.free(p)
	}
	a.ptrs = nil
}

// fillSaveRequest fills creq for game running under core; every buffer it
// points at is owned by a.
func fillSaveRequest(a *cAllocs, creq *C.sigil_save_request, game *Result, core, contentPath string,
	listing []string, options map[string]string) {
	cresult := (*C.sigil_result)(a.alloc(C.sizeof_sigil_result))
	cresult.struct_version = C.SIGIL_RESULT_V3
	cresult.features = C.uint32_t(game.Features)
	cresult.platform = C.sigil_platform(game.Platform)
	copyChars(cresult.title_id[:], game.TitleID)
	copyChars(cresult.save_id[:], game.SaveID)

	creq.struct_version = C.SIGIL_SAVE_REQUEST_V1
	creq.layout = a.str(core)
	if game.PlatformSlug != "" {
		creq.platform = a.str(game.PlatformSlug)
	}
	creq.content_path = a.str(contentPath)
	creq.result = cresult
	creq.features = C.uint32_t(game.Features)

	keys := make([]string, 0, len(options))
	for k := range options {
		keys = append(keys, k)
	}
	sort.Strings(keys)
	coptions := (*C.sigil_save_option)(a.alloc(C.size_t(len(keys)+1) * C.sizeof_sigil_save_option))
	copts := unsafe.Slice(coptions, len(keys))
	for i, key := range keys {
		copts[i].key = a.str(key)
		copts[i].value = a.str(options[key])
	}
	creq.options = coptions
	creq.option_count = C.size_t(len(keys))
	creq.listing = a.strings(listing)
	creq.listing_count = C.size_t(len(listing))
}

// LocateSaves returns the files under a save root that belong to game when
// core runs contentPath. Names only; no file is read. opts may be nil.
// docs/go.md defines every input.
func LocateSaves(game *Result, core, contentPath string, opts *LocateOptions) (*SaveUnit, error) {
	if game == nil {
		return nil, ErrInvalidArg
	}
	if opts == nil {
		opts = &LocateOptions{}
	}
	listing := opts.Listing
	if listing == nil && opts.SaveRoot != "" {
		var err error
		if listing, err = ListSaveRoot(opts.SaveRoot, core); err != nil {
			return nil, err
		}
	}

	var a cAllocs
	defer a.free()
	creq := (*C.sigil_save_request)(a.alloc(C.sizeof_sigil_save_request))
	fillSaveRequest(&a, creq, game, core, contentPath, listing, opts.Options)
	C.sigil_go_set_open(creq, nil)

	var cunit *C.sigil_save_unit
	rc := C.sigil_save_resolve(creq, &cunit)
	if err := errFromCode(rc); err != nil {
		return nil, err
	}
	defer C.sigil_save_unit_free(cunit)

	unit := &SaveUnit{
		Key:      C.GoString(&cunit.key[0]),
		Shape:    SaveShape(cunit.shape),
		Members:  goMembers(cunit.members, int(cunit.member_count)),
		Expected: goMembers(cunit.expected, int(cunit.expected_count)),
		Artifact: C.GoString(&cunit.artifact[0]),
	}
	for i := 0; i < int(cunit.unkeyed_count); i++ {
		item := (*[C.SIGIL_SAVE_PATH_MAX]C.char)(unsafe.Add(unsafe.Pointer(cunit.unkeyed), uintptr(i)*C.SIGIL_SAVE_PATH_MAX))
		unit.Unkeyed = append(unit.Unkeyed, C.GoString(&item[0]))
	}
	return unit, nil
}

// HashSaves fills unit.ContentHash and unit.IdentityHash from the files
// under saveRoot.
func HashSaves(unit *SaveUnit, saveRoot string) error {
	if unit == nil || saveRoot == "" {
		return ErrInvalidArg
	}
	if len(unit.Members) == 0 {
		return nil
	}

	cmembers := (*C.sigil_save_member)(C.calloc(C.size_t(len(unit.Members)), C.sizeof_sigil_save_member))
	defer C.free(unsafe.Pointer(cmembers))
	members := unsafe.Slice(cmembers, len(unit.Members))
	for i := range unit.Members {
		copyChars(members[i].path[:], unit.Members[i].Path)
		copyChars(members[i].entry[:], unit.Members[i].Entry)
		members[i].role = C.int(unit.Members[i].Role)
		members[i].present = 1
	}

	cunit := (*C.sigil_save_unit)(C.calloc(1, C.sizeof_sigil_save_unit))
	defer C.free(unsafe.Pointer(cunit))
	cunit.struct_version = C.SIGIL_SAVE_UNIT_V1
	cunit.shape = C.int(unit.Shape)
	copyChars(cunit.key[:], unit.Key)
	cunit.members = cmembers
	cunit.member_count = C.size_t(len(unit.Members))

	croot := C.CString(saveRoot)
	defer C.free(unsafe.Pointer(croot))
	if err := errFromCode(C.sigil_go_hash(cunit, croot)); err != nil {
		return err
	}
	unit.ContentHash = C.GoString(&cunit.content_hash[0])
	unit.IdentityHash = C.GoString(&cunit.identity_hash[0])
	return nil
}

func copyChars(dst []C.char, s string) {
	n := len(s)
	if n > len(dst)-1 {
		n = len(dst) - 1
	}
	for i := 0; i < n; i++ {
		dst[i] = C.char(s[i])
	}
	dst[n] = 0
}

func goMembers(members *C.sigil_save_member, count int) []SaveMember {
	if count == 0 {
		return nil
	}
	out := make([]SaveMember, 0, count)
	for _, m := range unsafe.Slice(members, count) {
		out = append(out, SaveMember{
			Path:    C.GoString(&m.path[0]),
			Entry:   C.GoString(&m.entry[0]),
			Role:    SaveRole(m.role),
			Present: m.present != 0,
		})
	}
	return out
}

// CardFormat is the file format a memory card was read from.
type CardFormat int

const (
	CardFormatUnknown      CardFormat = C.SIGIL_CARD_FORMAT_UNKNOWN
	CardFormatPS1Raw       CardFormat = C.SIGIL_CARD_FORMAT_PS1_RAW
	CardFormatPS1GME       CardFormat = C.SIGIL_CARD_FORMAT_PS1_GME
	CardFormatPS1VMP       CardFormat = C.SIGIL_CARD_FORMAT_PS1_VMP
	CardFormatPS2          CardFormat = C.SIGIL_CARD_FORMAT_PS2
	CardFormatGameCubeRaw  CardFormat = C.SIGIL_CARD_FORMAT_GAMECUBE_RAW
	CardFormatDreamcastVMU CardFormat = C.SIGIL_CARD_FORMAT_DREAMCAST_VMU
	CardFormatSaturnBackup CardFormat = C.SIGIL_CARD_FORMAT_SATURN_BACKUP
	CardFormatSegaCDBRAM   CardFormat = C.SIGIL_CARD_FORMAT_SEGACD_BRAM
)

func (f CardFormat) String() string {
	switch f {
	case CardFormatPS1Raw:
		return "ps1-raw"
	case CardFormatPS1GME:
		return "ps1-gme"
	case CardFormatPS1VMP:
		return "ps1-vmp"
	case CardFormatPS2:
		return "ps2"
	case CardFormatGameCubeRaw:
		return "gamecube-raw"
	case CardFormatDreamcastVMU:
		return "dreamcast-vmu"
	case CardFormatSaturnBackup:
		return "saturn-backup"
	case CardFormatSegaCDBRAM:
		return "segacd-bram"
	default:
		return "unknown"
	}
}

// CardEntry is one save on a memory card. OwnerID is the product code it
// carries, or "" when it has none.
type CardEntry struct {
	Name       string
	OwnerID    string
	Blocks     uint32
	FirstBlock uint32
}

// CardListing is the saves on a memory card and the space left on it.
type CardListing struct {
	Format       CardFormat
	TotalBlocks  uint32
	FreeBlocks   uint32
	FreeSlots    uint32
	CorruptCount uint32
	Entries      []CardEntry
}

// ListCard returns the saves on the memory card at path. The card format is
// detected from its content.
func ListCard(path string) (*CardListing, error) {
	cpath := C.CString(path)
	defer C.free(unsafe.Pointer(cpath))
	io := C.sigil_io_open_file(cpath)
	if io == nil {
		return nil, ErrIO
	}
	defer C.sigil_io_close(io)

	var clisting *C.sigil_card_listing
	if err := errFromCode(C.sigil_card_list(io, &clisting)); err != nil {
		return nil, err
	}
	defer C.sigil_card_listing_free(clisting)

	listing := &CardListing{
		Format:       CardFormat(clisting.format),
		TotalBlocks:  uint32(clisting.total_blocks),
		FreeBlocks:   uint32(clisting.free_blocks),
		FreeSlots:    uint32(clisting.free_slots),
		CorruptCount: uint32(clisting.corrupt_count),
	}
	for _, e := range unsafe.Slice(clisting.entries, int(clisting.entry_count)) {
		listing.Entries = append(listing.Entries, CardEntry{
			Name:       C.GoString(&e.name[0]),
			OwnerID:    C.GoString(&e.owner_id[0]),
			Blocks:     uint32(e.blocks),
			FirstBlock: uint32(e.first_block),
		})
	}
	return listing, nil
}

// SyncOptions are the optional inputs to Collect and Restore. Listing
// (root-relative paths) replaces listing the save root; GameIDs are every id
// the game's saves may carry (all discs of a set); State is what the last call
// returned for this game.
type SyncOptions struct {
	Listing        []string
	Options        map[string]string
	GameIDs        []string
	State          []byte
	Unmanaged      bool
	OverwriteLocal bool
	Claimed        []string    // Saturn, Sega CD: names from Unowned the user said belong to this game.
	Companions     []Companion // Games whose saves this game reads, in the order they go on.
}

// Companion is a game whose saves this game reads, as a sequel reads its
// prequel's. Unit is its unit from RomM for Restore, or nil to leave its
// saves as they are.
type Companion struct {
	GameIDs []string
	Unit    []byte
}

// CompanionResult is what Collect found of a companion's saves with the
// game's, as the companion's unit. Data is nil when none are there.
type CompanionResult struct {
	Data         []byte
	ContentHash  string
	IdentityHash string
	Changed      bool
}

// OverflowError is the ErrNoSpace Restore returns when the saves don't fit:
// Name is the save that didn't, Blocks the blocks it lacked (0 when a
// directory slot was missing instead).
type OverflowError struct {
	Name   string
	Blocks uint32
}

func (e *OverflowError) Error() string {
	return fmt.Sprintf("sigil: not enough free space for %s (%d blocks short)", e.Name, e.Blocks)
}

func (e *OverflowError) Unwrap() error { return ErrNoSpace }

// SyncResult is what Collect or Restore produced. Store State and pass it to
// the next call for this game.
type SyncResult struct {
	Artifact     string
	Shape        SaveShape
	Data         []byte
	ContentHash  string
	IdentityHash string
	Changed      bool
	Conflict     bool
	State        []byte
	Holding      []byte            // Saturn, Sega CD: zip of the saves on a shared volume with no known owner.
	Unowned      []string          // The names of the saves in Holding.
	RestoreAgain bool              // Unmanaged: the saves the last Restore wrote were overwritten.
	Companions   []CompanionResult // Collect: one per SyncOptions.Companions, in order.
}

func runSync(unit []byte, game *Result, core, contentPath, saveRoot string, opts *SyncOptions) (*SyncResult, error) {
	if game == nil || saveRoot == "" {
		return nil, ErrInvalidArg
	}
	if opts == nil {
		opts = &SyncOptions{}
	}
	listing := opts.Listing
	if listing == nil {
		var err error
		if listing, err = ListSaveRoot(saveRoot, core); err != nil {
			return nil, err
		}
	}

	var a cAllocs
	defer a.free()
	creq := (*C.sigil_sync_request)(a.alloc(C.sizeof_sigil_sync_request))
	creq.struct_version = C.SIGIL_SYNC_REQUEST_V1
	fillSaveRequest(&a, &creq.save, game, core, contentPath, listing, opts.Options)
	creq.game_ids = a.strings(opts.GameIDs)
	creq.game_id_count = C.size_t(len(opts.GameIDs))
	creq.claimed = a.strings(opts.Claimed)
	creq.claimed_count = C.size_t(len(opts.Claimed))
	if n := len(opts.Companions); n > 0 {
		companions := (*C.sigil_sync_companion)(a.alloc(C.size_t(n) * C.sizeof_sigil_sync_companion))
		slots := unsafe.Slice(companions, n)
		for i := range slots {
			k := &slots[i]
			k.game_ids = a.strings(opts.Companions[i].GameIDs)
			k.game_id_count = C.size_t(len(opts.Companions[i].GameIDs))
			if unit := opts.Companions[i].Unit; unit != nil {
				buf := a.alloc(C.size_t(len(unit) + 1))
				copy(unsafe.Slice((*byte)(buf), len(unit)), unit)
				k.unit = (*C.uint8_t)(buf)
				k.unit_len = C.size_t(len(unit))
			}
		}
		creq.companions = companions
		creq.companion_count = C.size_t(n)
	}
	if opts.Unmanaged {
		creq.mode = C.SIGIL_SYNC_UNMANAGED
	}
	if opts.OverwriteLocal {
		creq.overwrite_local = 1
	}
	if len(opts.State) > 0 {
		state := a.alloc(C.size_t(len(opts.State)))
		copy(unsafe.Slice((*byte)(state), len(opts.State)), opts.State)
		creq.state = (*C.uint8_t)(state)
		creq.state_len = C.size_t(len(opts.State))
	}
	C.sigil_go_set_sync_io(creq, a.str(saveRoot))

	var cres *C.sigil_sync_result
	var rc C.int
	if unit == nil {
		rc = C.sigil_collect(creq, &cres)
	} else {
		cunit := a.alloc(C.size_t(len(unit) + 1))
		copy(unsafe.Slice((*byte)(cunit), len(unit)), unit)
		rc = C.sigil_restore(creq, (*C.uint8_t)(cunit), C.size_t(len(unit)), &cres)
	}
	if cres != nil {
		defer C.sigil_sync_result_free(cres)
	}
	if rc == C.SIGIL_ERR_NO_SPACE && cres != nil {
		return nil, &OverflowError{Name: C.GoString(&cres.overflow[0]), Blocks: uint32(cres.overflow_blocks)}
	}
	if err := errFromCode(rc); err != nil {
		return nil, err
	}
	out := &SyncResult{
		Artifact:     C.GoString(&cres.artifact[0]),
		Shape:        SaveShape(cres.shape),
		ContentHash:  C.GoString(&cres.content_hash[0]),
		IdentityHash: C.GoString(&cres.identity_hash[0]),
		Changed:      cres.changed != 0,
		Conflict:     cres.conflict != 0,
		RestoreAgain: cres.restore_again != 0,
	}
	if cres.data != nil {
		out.Data = C.GoBytes(unsafe.Pointer(cres.data), C.int(cres.len))
	}
	if cres.state != nil {
		out.State = C.GoBytes(unsafe.Pointer(cres.state), C.int(cres.state_len))
	}
	if cres.holding != nil {
		out.Holding = C.GoBytes(unsafe.Pointer(cres.holding), C.int(cres.holding_len))
	}
	for _, name := range unsafe.Slice(cres.unowned, cres.unowned_count) {
		out.Unowned = append(out.Unowned, C.GoString(&name[0]))
	}
	for _, c := range unsafe.Slice(cres.companions, cres.companion_count) {
		result := CompanionResult{
			ContentHash:  C.GoString(&c.content_hash[0]),
			IdentityHash: C.GoString(&c.identity_hash[0]),
			Changed:      c.changed != 0,
		}
		if c.data != nil {
			result.Data = C.GoBytes(unsafe.Pointer(c.data), C.int(c.len))
		}
		out.Companions = append(out.Companions, result)
	}
	return out, nil
}

// Collect gathers game's saves under saveRoot into the unit that travels to
// RomM. Store the returned State once the unit reached RomM. docs/go.md
// defines every input.
func Collect(game *Result, core, contentPath, saveRoot string, opts *SyncOptions) (*SyncResult, error) {
	return runSync(nil, game, core, contentPath, saveRoot, opts)
}

// Restore puts unit back under saveRoot and reads it back. It returns
// ErrConflict, writing nothing, when the saves there changed since the last
// sync and opts.OverwriteLocal is false.
func Restore(unit []byte, game *Result, core, contentPath, saveRoot string, opts *SyncOptions) (*SyncResult, error) {
	if unit == nil {
		return nil, ErrInvalidArg
	}
	return runSync(unit, game, core, contentPath, saveRoot, opts)
}

// Version returns the sigil C library version.
func Version() string { return C.GoString(C.sigil_version()) }
