// SPDX-License-Identifier: MPL-2.0
package com.nendo.sigil;

import java.util.List;

/**
 * A SigilFileAccess written in Java: its four methods and nothing more, as an app's own access
 * written in Java has them. It counts its whole-file writes.
 */
public final class JavaFileAccess implements SigilFileAccess {
    int writes;

    @Override
    public List<SigilFileEntry> list(String root, String path) {
        return PosixFileAccess.INSTANCE.list(root, path);
    }

    @Override
    public byte[] read(String root, String path) {
        return PosixFileAccess.INSTANCE.read(root, path);
    }

    @Override
    public boolean write(String root, String path, byte[] data) {
        writes++;
        return PosixFileAccess.INSTANCE.write(root, path, data);
    }

    @Override
    public boolean remove(String root, String path) {
        return PosixFileAccess.INSTANCE.remove(root, path);
    }
}
