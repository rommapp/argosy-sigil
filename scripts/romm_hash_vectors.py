"""Golden vectors for sigil's save-unit hash, produced with RomM's own
algorithm (backend/handler/filesystem/assets_handler.py compute_content_hash)."""
import hashlib
import io
import zipfile


def file_hash(data: bytes) -> str:
    return hashlib.md5(data).hexdigest()


def zip_hash(zip_bytes: bytes) -> str:
    with zipfile.ZipFile(io.BytesIO(zip_bytes), "r") as zf:
        file_hashes = []
        for name in sorted(zf.namelist()):
            if not name.endswith("/"):
                content = zf.read(name)
                file_hashes.append(f"{name}:{hashlib.md5(content).hexdigest()}")
        combined = "\n".join(file_hashes)
        return hashlib.md5(combined.encode()).hexdigest()


def build_zip(entries, compression=zipfile.ZIP_STORED) -> bytes:
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w", compression=compression) as zf:
        for name, data in entries:
            if data is None:
                zf.writestr(name, b"")
            else:
                zf.writestr(name, data)
    return buf.getvalue()


def c_array(name: str, data: bytes) -> str:
    body = ", ".join(f"0x{b:02x}" for b in data)
    return f"static const uint8_t {name}[] = {{ {body} }};"


print("raw hello:", file_hash(b"hello"))

multi = [("Crystal.srm", b"AAAA"), ("Crystal.rtc", b"BBBB")]
print("multi Crystal.srm+Crystal.rtc:", zip_hash(build_zip(multi)))

folder = [("ULUS10064DATA00/", None), ("ULUS10064DATA00/PARAM.SFO", b"sfo"), ("ULUS10064DATA00/DATA.BIN", b"data")]
print("folder ULUS10064DATA00:", zip_hash(build_zip(folder)))

stored = build_zip([("a.txt", b"hello"), ("dir/", None), ("b.bin", b"\x00\x01\x02")], zipfile.ZIP_STORED)
print("stored zip hash:", zip_hash(stored))
print(c_array("STORED_ZIP", stored))

deflated = build_zip([("game.sav", b"x" * 5000), ("notes.txt", b"the quick brown fox")], zipfile.ZIP_DEFLATED)
print("deflated zip hash:", zip_hash(deflated))
print(c_array("DEFLATED_ZIP", deflated))
