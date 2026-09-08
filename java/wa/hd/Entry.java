package wa.hd;

import android.app.Application;
import android.content.pm.ApplicationInfo;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.lang.reflect.Method;
import java.lang.reflect.Modifier;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.Enumeration;
import java.util.HashMap;
import java.util.HashSet;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;

/**
 * Minimal runtime payload for WAStatusHD v0.4.
 *
 * This deliberately does not depend on LSPosed/Xposed or DexKit. It scans the
 * WhatsApp DEX files for the same stable strings WaEnhancer uses to resolve the
 * central Boolean/Integer property getters, then asks the native LSPlant bridge
 * to hook those Java methods.
 */
public final class Entry {
    private static final String TAG = "v0.4";

    private static final Map<Integer, Boolean> BOOL_OVERRIDES;
    private static final Map<Integer, Integer> INT_OVERRIDES;
    private static final Set<String> LOGGED_KEYS = Collections.synchronizedSet(new HashSet<String>());

    static {
        Map<Integer, Boolean> b = new HashMap<>();
        b.put(14447, false); // disable manual ProcessMediaQuality calculation
        b.put(5549, true);   // high quality video path
        b.put(18888, true);  // transcoder path
        b.put(6033, true);   // image quality flags
        b.put(9569, false);
        b.put(26289, true);
        b.put(22375, true);
        b.put(7589, true);   // media quality selection
        b.put(6972, false);
        BOOL_OVERRIDES = Collections.unmodifiableMap(b);

        Map<Integer, Integer> i = new HashMap<>();
        for (int id : new int[]{594, 12852, 4686, 3654, 3183, 4685}) i.put(id, 1920);
        for (int id : new int[]{3755, 3756, 3757, 3758}) i.put(id, 10000);
        for (int id : new int[]{1577, 6030, 2656, 15752, 15746}) i.put(id, 50 * 1024);
        for (int id : new int[]{1581, 1575, 1578, 6029, 2655, 15749}) i.put(id, 100);
        for (int id : new int[]{1576, 2654, 6032, 15748, 3068}) i.put(id, 3840);
        INT_OVERRIDES = Collections.unmodifiableMap(i);
    }

    private Entry() {}

    private static native Method nativeHook(Method target, Object hooker, Method callback);
    private static native boolean nativeDeoptimize(Method target);
    private static native void nativeLog(String message);

    public static boolean start(Application app) {
        try {
            if (app == null) {
                log("FAIL app=null");
                return false;
            }
            ApplicationInfo ai = app.getApplicationInfo();
            ClassLoader targetLoader = app.getClassLoader();
            List<String> apkPaths = new ArrayList<>();
            if (ai.sourceDir != null) apkPaths.add(ai.sourceDir);
            if (ai.splitSourceDirs != null) apkPaths.addAll(Arrays.asList(ai.splitSourceDirs));

            log("resolver start apks=" + apkPaths.size() + " loader=" + targetLoader.getClass().getName());

            ResolvedMethod boolResolved = findMethod(apkPaths, "Unknown BooleanField");
            ResolvedMethod intResolved = findMethod(apkPaths, "Unknown IntField");

            if (boolResolved == null || intResolved == null) {
                log("FAIL resolver bool=" + boolResolved + " int=" + intResolved);
                return false;
            }

            Method boolMethod = boolResolved.toReflection(targetLoader);
            Method intMethod = intResolved.toReflection(targetLoader);
            if (boolMethod == null || intMethod == null) {
                log("FAIL reflection bool=" + boolResolved + " int=" + intResolved);
                return false;
            }

            log("resolved BOOL " + describe(boolMethod));
            log("resolved INT  " + describe(intMethod));

            PropHook boolHook = new PropHook(true, Modifier.isStatic(boolMethod.getModifiers()));
            PropHook intHook = new PropHook(false, Modifier.isStatic(intMethod.getModifiers()));
            Method callback = PropHook.class.getDeclaredMethod("callback", Object[].class);
            callback.setAccessible(true);

            Method boolBackup = nativeHook(boolMethod, boolHook, callback);
            if (boolBackup != null) {
                boolBackup.setAccessible(true);
                boolHook.backup = boolBackup;
                nativeDeoptimize(boolMethod);
            }
            Method intBackup = nativeHook(intMethod, intHook, callback);
            if (intBackup != null) {
                intBackup.setAccessible(true);
                intHook.backup = intBackup;
                nativeDeoptimize(intMethod);
            }

            boolean ok = boolBackup != null && intBackup != null;
            log(ok ? "HOOKED central property getters: bool+int" :
                    "FAIL LSPlant hook bool=" + (boolBackup != null) + " int=" + (intBackup != null));
            if (ok) {
                log("ACTIVE overrides bool=" + BOOL_OVERRIDES.size() + " int=" + INT_OVERRIDES.size());
            }
            return ok;
        } catch (Throwable t) {
            log("FAIL start " + t.getClass().getName() + ": " + String.valueOf(t.getMessage()));
            return false;
        }
    }

    public static final class PropHook {
        volatile Method backup;
        final boolean booleanKind;
        final boolean staticTarget;

        PropHook(boolean booleanKind, boolean staticTarget) {
            this.booleanKind = booleanKind;
            this.staticTarget = staticTarget;
        }

        public Object callback(Object[] args) throws Throwable {
            Integer id = firstInteger(args);
            if (id != null) {
                if (booleanKind) {
                    Boolean value = BOOL_OVERRIDES.get(id);
                    if (value != null) {
                        logOnce("B:" + id, "APPLIED bool id=" + id + " value=" + value);
                        return value;
                    }
                } else {
                    Integer value = INT_OVERRIDES.get(id);
                    if (value != null) {
                        logOnce("I:" + id, "APPLIED int id=" + id + " value=" + value);
                        return value;
                    }
                }
            }

            Method b = backup;
            if (b == null) {
                throw new IllegalStateException("backup unavailable");
            }
            if (staticTarget) {
                return b.invoke(null, args == null ? new Object[0] : args);
            }
            if (args == null || args.length == 0) {
                throw new IllegalStateException("instance target without receiver");
            }
            Object receiver = args[0];
            Object[] params = Arrays.copyOfRange(args, 1, args.length);
            return b.invoke(receiver, params);
        }
    }

    private static Integer firstInteger(Object[] args) {
        if (args == null) return null;
        for (Object arg : args) {
            if (arg instanceof Integer) return (Integer) arg;
        }
        return null;
    }

    private static void logOnce(String key, String message) {
        if (LOGGED_KEYS.add(key)) log(message);
    }

    private static void log(String message) {
        try {
            nativeLog(TAG + " " + message);
        } catch (Throwable ignored) {
            // Native logger is diagnostic only; never crash WhatsApp because of logging.
        }
    }

    private static String describe(Method m) {
        return m.getDeclaringClass().getName() + "->" + m.getName() + reflectionDescriptor(m);
    }

    private static ResolvedMethod findMethod(List<String> apkPaths, String needle) {
        LinkedHashSet<ResolvedMethod> matches = new LinkedHashSet<>();
        for (String apk : apkPaths) {
            if (apk == null) continue;
            try (ZipFile zip = new ZipFile(apk)) {
                Enumeration<? extends ZipEntry> entries = zip.entries();
                while (entries.hasMoreElements()) {
                    ZipEntry e = entries.nextElement();
                    String n = e.getName();
                    if (!n.startsWith("classes") || !n.endsWith(".dex")) continue;
                    byte[] dex;
                    try (InputStream in = zip.getInputStream(e)) {
                        dex = readAll(in);
                    }
                    DexScanner scanner = new DexScanner(dex);
                    matches.addAll(scanner.findUsingString(needle));
                }
            } catch (Throwable t) {
                log("scan warning " + apk + " " + t.getClass().getSimpleName());
            }
        }
        if (matches.isEmpty()) return null;
        for (ResolvedMethod rm : matches) {
            if (rm.descriptor != null && rm.descriptor.length() > 0) return rm;
        }
        return matches.iterator().next();
    }

    private static byte[] readAll(InputStream in) throws IOException {
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        byte[] buf = new byte[16384];
        int r;
        while ((r = in.read(buf)) != -1) out.write(buf, 0, r);
        return out.toByteArray();
    }

    static final class ResolvedMethod {
        final String classDescriptor;
        final String name;
        final String descriptor;

        ResolvedMethod(String classDescriptor, String name, String descriptor) {
            this.classDescriptor = classDescriptor;
            this.name = name;
            this.descriptor = descriptor;
        }

        Method toReflection(ClassLoader loader) {
            try {
                String className = classDescriptor;
                if (className.startsWith("L") && className.endsWith(";")) {
                    className = className.substring(1, className.length() - 1).replace('/', '.');
                }
                Class<?> cls = Class.forName(className, false, loader);
                for (Method m : cls.getDeclaredMethods()) {
                    if (!m.getName().equals(name)) continue;
                    if (!reflectionDescriptor(m).equals(descriptor)) continue;
                    m.setAccessible(true);
                    return m;
                }
            } catch (Throwable t) {
                log("reflection warning " + this + " " + t.getClass().getSimpleName());
            }
            return null;
        }

        @Override public String toString() {
            return classDescriptor + "->" + name + descriptor;
        }

        @Override public boolean equals(Object o) {
            if (!(o instanceof ResolvedMethod)) return false;
            ResolvedMethod r = (ResolvedMethod) o;
            return classDescriptor.equals(r.classDescriptor) && name.equals(r.name) && descriptor.equals(r.descriptor);
        }

        @Override public int hashCode() {
            return classDescriptor.hashCode() * 31 * 31 + name.hashCode() * 31 + descriptor.hashCode();
        }
    }

    private static String reflectionDescriptor(Method m) {
        StringBuilder sb = new StringBuilder("(");
        for (Class<?> p : m.getParameterTypes()) sb.append(typeDescriptor(p));
        sb.append(')').append(typeDescriptor(m.getReturnType()));
        return sb.toString();
    }

    private static String typeDescriptor(Class<?> c) {
        if (c.isArray()) return c.getName().replace('.', '/');
        if (!c.isPrimitive()) return "L" + c.getName().replace('.', '/') + ";";
        if (c == void.class) return "V";
        if (c == boolean.class) return "Z";
        if (c == byte.class) return "B";
        if (c == char.class) return "C";
        if (c == short.class) return "S";
        if (c == int.class) return "I";
        if (c == long.class) return "J";
        if (c == float.class) return "F";
        if (c == double.class) return "D";
        throw new AssertionError(c);
    }

    /** Small DEX parser focused only on method/string cross references. */
    static final class DexScanner {
        final byte[] d;
        final int stringIdsSize, stringIdsOff;
        final int typeIdsSize, typeIdsOff;
        final int protoIdsSize, protoIdsOff;
        final int methodIdsSize, methodIdsOff;
        final int classDefsSize, classDefsOff;
        String[] strings;
        String[] types;

        DexScanner(byte[] d) {
            this.d = d;
            if (d.length < 112 || d[0] != 'd' || d[1] != 'e' || d[2] != 'x') {
                throw new IllegalArgumentException("not dex");
            }
            stringIdsSize = u4(56); stringIdsOff = u4(60);
            typeIdsSize = u4(64); typeIdsOff = u4(68);
            protoIdsSize = u4(72); protoIdsOff = u4(76);
            methodIdsSize = u4(88); methodIdsOff = u4(92);
            classDefsSize = u4(96); classDefsOff = u4(100);
        }

        List<ResolvedMethod> findUsingString(String needle) {
            ensureStrings();
            Set<Integer> targetStrings = new HashSet<>();
            for (int i = 0; i < strings.length; i++) {
                String s = strings[i];
                if (s != null && s.contains(needle)) targetStrings.add(i);
            }
            if (targetStrings.isEmpty()) return Collections.emptyList();

            List<ResolvedMethod> out = new ArrayList<>();
            for (int c = 0; c < classDefsSize; c++) {
                int off = classDefsOff + c * 32;
                if (!valid(off, 32)) continue;
                int classDataOff = u4(off + 24);
                if (classDataOff == 0 || !valid(classDataOff, 1)) continue;
                scanClassData(classDataOff, targetStrings, out);
            }
            return out;
        }

        private void scanClassData(int offset, Set<Integer> targets, List<ResolvedMethod> out) {
            Pos p = new Pos(offset);
            int staticFields = uleb(p);
            int instanceFields = uleb(p);
            int directMethods = uleb(p);
            int virtualMethods = uleb(p);
            for (int i = 0; i < staticFields + instanceFields; i++) { uleb(p); uleb(p); }
            scanMethods(p, directMethods, targets, out);
            scanMethods(p, virtualMethods, targets, out);
        }

        private void scanMethods(Pos p, int count, Set<Integer> targets, List<ResolvedMethod> out) {
            int methodIdx = 0;
            for (int i = 0; i < count; i++) {
                methodIdx += uleb(p);
                uleb(p); // access_flags
                int codeOff = uleb(p);
                if (codeOff != 0 && methodIdx >= 0 && methodIdx < methodIdsSize && codeUsesString(codeOff, targets)) {
                    ResolvedMethod rm = resolveMethodId(methodIdx);
                    if (rm != null) out.add(rm);
                }
            }
        }

        private boolean codeUsesString(int codeOff, Set<Integer> targets) {
            if (!valid(codeOff, 16)) return false;
            int insnsSize = u4(codeOff + 12);
            int start = codeOff + 16;
            if (insnsSize < 0 || !valid(start, insnsSize * 2L)) return false;
            for (int i = 0; i < insnsSize; i++) {
                int unit = u2(start + i * 2);
                int opcode = unit & 0xff;
                if (opcode == 0x1a && i + 1 < insnsSize) { // const-string vAA, string@BBBB
                    int idx = u2(start + (i + 1) * 2);
                    if (targets.contains(idx)) return true;
                } else if (opcode == 0x1b && i + 2 < insnsSize) { // const-string/jumbo
                    long lo = u2(start + (i + 1) * 2);
                    long hi = u2(start + (i + 2) * 2);
                    long idx = lo | (hi << 16);
                    if (idx <= Integer.MAX_VALUE && targets.contains((int) idx)) return true;
                }
            }
            return false;
        }

        private ResolvedMethod resolveMethodId(int idx) {
            ensureTypes();
            int off = methodIdsOff + idx * 8;
            if (!valid(off, 8)) return null;
            int classIdx = u2(off);
            int protoIdx = u2(off + 2);
            int nameIdx = u4(off + 4);
            if (classIdx < 0 || classIdx >= types.length || nameIdx < 0 || nameIdx >= strings.length || protoIdx < 0 || protoIdx >= protoIdsSize) return null;
            String classDesc = types[classIdx];
            String name = strings[nameIdx];
            String descriptor = protoDescriptor(protoIdx);
            if (classDesc == null || name == null || descriptor == null) return null;
            return new ResolvedMethod(classDesc, name, descriptor);
        }

        private String protoDescriptor(int protoIdx) {
            ensureTypes();
            int off = protoIdsOff + protoIdx * 12;
            if (!valid(off, 12)) return null;
            int returnTypeIdx = u4(off + 4);
            int paramsOff = u4(off + 8);
            if (returnTypeIdx < 0 || returnTypeIdx >= types.length) return null;
            StringBuilder sb = new StringBuilder("(");
            if (paramsOff != 0) {
                if (!valid(paramsOff, 4)) return null;
                int size = u4(paramsOff);
                long bytes = 4L + size * 2L;
                if (size < 0 || !valid(paramsOff, bytes)) return null;
                for (int i = 0; i < size; i++) {
                    int typeIdx = u2(paramsOff + 4 + i * 2);
                    if (typeIdx < 0 || typeIdx >= types.length) return null;
                    sb.append(types[typeIdx]);
                }
            }
            sb.append(')').append(types[returnTypeIdx]);
            return sb.toString();
        }

        private void ensureStrings() {
            if (strings != null) return;
            strings = new String[stringIdsSize];
            for (int i = 0; i < stringIdsSize; i++) {
                int idOff = stringIdsOff + i * 4;
                if (!valid(idOff, 4)) continue;
                int dataOff = u4(idOff);
                strings[i] = readDexString(dataOff);
            }
        }

        private void ensureTypes() {
            ensureStrings();
            if (types != null) return;
            types = new String[typeIdsSize];
            for (int i = 0; i < typeIdsSize; i++) {
                int off = typeIdsOff + i * 4;
                if (!valid(off, 4)) continue;
                int stringIdx = u4(off);
                if (stringIdx >= 0 && stringIdx < strings.length) types[i] = strings[stringIdx];
            }
        }

        private String readDexString(int off) {
            if (!valid(off, 1)) return null;
            Pos p = new Pos(off);
            uleb(p); // utf16 length; not needed for our ASCII resolver strings/descriptors
            int start = p.v;
            int end = start;
            while (end < d.length && d[end] != 0) end++;
            if (end >= d.length) return null;
            return new String(d, start, end - start, StandardCharsets.UTF_8);
        }

        private int uleb(Pos p) {
            int result = 0;
            int shift = 0;
            for (int n = 0; n < 5; n++) {
                if (!valid(p.v, 1)) return 0;
                int b = d[p.v++] & 0xff;
                result |= (b & 0x7f) << shift;
                if ((b & 0x80) == 0) return result;
                shift += 7;
            }
            return result;
        }

        private int u2(int off) {
            if (!valid(off, 2)) return 0;
            return (d[off] & 0xff) | ((d[off + 1] & 0xff) << 8);
        }

        private int u4(int off) {
            if (!valid(off, 4)) return 0;
            return (d[off] & 0xff) |
                    ((d[off + 1] & 0xff) << 8) |
                    ((d[off + 2] & 0xff) << 16) |
                    ((d[off + 3] & 0xff) << 24);
        }

        private boolean valid(long off, long len) {
            return off >= 0 && len >= 0 && off + len <= d.length;
        }

        static final class Pos { int v; Pos(int v) { this.v = v; } }
    }
}
