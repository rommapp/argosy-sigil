// SPDX-License-Identifier: MPL-2.0
package com.nendo.sigil;

import java.util.Collections;

/** Calls the binding from Java, in the shape Java code compiled before writesParts calls it. */
public final class JavaCalls {
    private JavaCalls() {}

    /** A restore with every input, as Java passes them: no writesParts. */
    public static SigilSyncResult restore(byte[] unit, SigilResult game, String core, String contentPath, String saveRoot,
                                          java.util.Map<String, String> options, SigilFileAccess fileAccess) {
        return Sigil.INSTANCE.restore(unit, game, core, contentPath, saveRoot, null, options, Collections.emptyList(),
                null, false, false, Collections.emptyList(), Collections.emptyList(), false, null, fileAccess);
    }
}
