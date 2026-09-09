// SPDX-License-Identifier: MPL-2.0

// Package sigil derives the platform-native title ID from a ROM file and
// resolves the save unit an emulator keeps for it.
package sigil

/*
#cgo CFLAGS: -I${SRCDIR}/../../include
#cgo LDFLAGS: -L${SRCDIR}/../../build -lsigil -lsigil_chdr -lsigil_zstd -lsigil_zlib -lsigil_lzma -lsigil_aes

#include <stdio.h>
#include <stdlib.h>
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

const subdirListDepth = 3

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

	var cstrings []*C.char
	cstr := func(s string) *C.char {
		p := C.CString(s)
		cstrings = append(cstrings, p)
		return p
	}
	defer func() {
		for _, p := range cstrings {
			C.free(unsafe.Pointer(p))
		}
	}()

	cresult := (*C.sigil_result)(C.calloc(1, C.sizeof_sigil_result))
	defer C.free(unsafe.Pointer(cresult))
	cresult.struct_version = C.SIGIL_RESULT_V3
	cresult.features = C.uint32_t(game.Features)
	copyChars(cresult.title_id[:], game.TitleID)
	copyChars(cresult.save_id[:], game.SaveID)

	creq := (*C.sigil_save_request)(C.calloc(1, C.sizeof_sigil_save_request))
	defer C.free(unsafe.Pointer(creq))
	creq.struct_version = C.SIGIL_SAVE_REQUEST_V1
	creq.layout = cstr(core)
	if game.PlatformSlug != "" {
		creq.platform = cstr(game.PlatformSlug)
	}
	creq.content_path = cstr(contentPath)
	creq.result = cresult
	creq.features = C.uint32_t(game.Features)

	keys := make([]string, 0, len(opts.Options))
	for k := range opts.Options {
		keys = append(keys, k)
	}
	sort.Strings(keys)
	coptions := (*C.sigil_save_option)(C.calloc(C.size_t(len(keys)+1), C.sizeof_sigil_save_option))
	defer C.free(unsafe.Pointer(coptions))
	copts := unsafe.Slice(coptions, len(keys))
	for i, key := range keys {
		copts[i].key = cstr(key)
		copts[i].value = cstr(opts.Options[key])
	}
	creq.options = coptions
	creq.option_count = C.size_t(len(keys))

	clisting := (**C.char)(C.calloc(C.size_t(len(listing)+1), C.size_t(unsafe.Sizeof((*C.char)(nil)))))
	defer C.free(unsafe.Pointer(clisting))
	paths := unsafe.Slice(clisting, len(listing))
	for i, p := range listing {
		paths[i] = cstr(p)
	}
	creq.listing = clisting
	creq.listing_count = C.size_t(len(listing))
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

// Version returns the sigil C library version.
func Version() string { return C.GoString(C.sigil_version()) }
