package com.toolscreen.macos;

import java.awt.Color;
import java.awt.Container;
import java.awt.Cursor;
import java.awt.Font;
import java.awt.Frame;
import java.awt.Graphics;
import java.awt.Graphics2D;
import java.awt.GraphicsEnvironment;
import java.awt.RenderingHints;
import java.awt.Window;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.lang.reflect.InvocationTargetException;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardCopyOption;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.regex.Matcher;
import java.util.regex.Pattern;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;

import javax.swing.BorderFactory;
import javax.swing.Box;
import javax.swing.BoxLayout;
import javax.swing.JButton;
import javax.swing.JDialog;
import javax.swing.JLabel;
import javax.swing.JOptionPane;
import javax.swing.JPanel;
import javax.swing.JProgressBar;
import javax.swing.SwingUtilities;
import javax.swing.UIManager;
import javax.swing.WindowConstants;

/**
 * Entry point of MacScreen.jar. Double-clicking the jar runs the installer.
 * The launcher runs ~/.config/toolscreen/MacScreen.jar before every launch
 * (the instance's pre-launch command): it runs the player's own pre-launch
 * command, then checks GitHub for a newer release and offers to install it,
 * like the updater in Toolscreen's Windows injector. The game has not started
 * yet, so the update is used for this launch.
 *
 * Compiled for Java 8 because the pre-launch command runs on the instance's
 * Java. The installer (Installer.java) needs Java 17.
 */
public final class Updater {
    static final String PROJECT_NAME = "MacScreen";
    static final String PRELAUNCH_ARG = "--prelaunch";
    static final String JAR_NAME = "MacScreen.jar";
    static final String DYLIB_NAME = "Toolscreen.dylib";
    static final String LOGGER_NAME = "liblogger.dylib";

    static final Color BG = new Color(43, 43, 43);
    static final Color FG = new Color(224, 224, 224);

    private static final String LATEST_RELEASE_API = "https://api.github.com/repos/Woblu/MacScreen/releases/latest";
    private static final String RELEASES_PAGE = "https://github.com/Woblu/MacScreen/releases";
    private static final String DOWNLOAD_PREFIX = "https://github.com/Woblu/MacScreen/releases/download/";
    private static final long MAX_DOWNLOAD_BYTES = 256L * 1024 * 1024;

    private Updater() {}

    public static void main(String[] args) {
        if (args.length > 0 && args[0].equals(PRELAUNCH_ARG)) {
            System.exit(preLaunch(Arrays.copyOfRange(args, 1, args.length)));
        }
        if (javaVersion() < 17) {
            JOptionPane.showMessageDialog(null, "Installing " + PROJECT_NAME + " needs Java 17 or newer.", PROJECT_NAME,
                JOptionPane.ERROR_MESSAGE);
            return;
        }
        try {
            Class.forName("com.toolscreen.macos.Installer").getMethod("main", String[].class).invoke(null, (Object) args);
        } catch (ReflectiveOperationException e) {
            throw new IllegalStateException(e);
        }
    }

    /** Runs the player's pre-launch command (the arguments after --prelaunch), then the update check. */
    private static int preLaunch(String[] command) {
        if (command.length > 0) {
            int code = run(command);
            if (code != 0) return code;
        }
        try {
            checkForUpdate();
        } catch (Throwable t) {
            // Never stop the game from launching.
            log("Update check failed: " + t);
        }
        return 0;
    }

    private static int run(String[] command) {
        try {
            return new ProcessBuilder(command).inheritIO().start().waitFor();
        } catch (IOException e) {
            log("Could not run the pre-launch command: " + e.getMessage());
            return 1;
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
            return 1;
        }
    }

    private static void checkForUpdate() throws IOException {
        if (GraphicsEnvironment.isHeadless()) return;
        String current = readVersion();
        Map<?, ?> release = (Map<?, ?>) Json.parse(httpGet(LATEST_RELEASE_API));
        String tag = text(release.get("tag_name"));
        String latest = tag.startsWith("v") ? tag.substring(1) : tag;
        if (compareVersions(latest, current) <= 0) {
            log("Up to date (" + current + ")");
            return;
        }
        Map<?, ?> asset = jarAsset(release.get("assets"));
        if (asset == null) {
            log("Release " + tag + " has no " + PROJECT_NAME + " jar");
            return;
        }
        log("Update available: " + current + " -> " + latest);

        System.setProperty("apple.awt.application.name", PROJECT_NAME);
        onEdt(Updater::applyDarkTheme);
        if (!confirmUpdate(current, latest)) return;

        Files.createDirectories(configDirectory().toPath());
        Path download = Files.createTempFile(configDirectory().toPath(), PROJECT_NAME + "-", ".jar.tmp");
        try {
            downloadWithProgress(text(asset.get("browser_download_url")), latest, download);
            String digest = text(asset.get("digest"));
            if (digest.startsWith("sha256:") && !digest.substring(7).equalsIgnoreCase(sha256(download))) {
                throw new IOException("the download is damaged.");
            }
            install(download, latest);
            log("Updated to " + latest);
        } catch (IOException e) {
            log("Update failed: " + e.getMessage());
            showMessage(PROJECT_NAME + " — Update", "Updating failed: " + escape(String.valueOf(e.getMessage()))
                + "<br>The game starts with v" + escape(current) + ".");
        } finally {
            Files.deleteIfExists(download);
        }
    }

    /** The release's MacScreen-<version>.jar, if it is downloaded from this repository. */
    private static Map<?, ?> jarAsset(Object assets) {
        if (!(assets instanceof List)) return null;
        for (Object item : (List<?>) assets) {
            if (!(item instanceof Map)) continue;
            Map<?, ?> asset = (Map<?, ?>) item;
            String name = text(asset.get("name"));
            if (name.startsWith(PROJECT_NAME + "-") && name.endsWith(".jar")
                && text(asset.get("browser_download_url")).startsWith(DOWNLOAD_PREFIX)) {
                return asset;
            }
        }
        return null;
    }

    /** Installs the downloaded jar's dylibs, then makes it the jar the launcher runs. */
    private static void install(Path jar, String version) throws IOException {
        try (ZipFile zip = new ZipFile(jar.toFile())) {
            ZipEntry versionEntry = zip.getEntry("payload/version.txt");
            String inside = "";
            if (versionEntry != null) {
                try (InputStream in = zip.getInputStream(versionEntry)) {
                    inside = new String(readAll(in), StandardCharsets.UTF_8).trim();
                }
            }
            if (!inside.equals(version)) throw new IOException("the download is not version " + version + ".");
            for (String name : new String[] { DYLIB_NAME, LOGGER_NAME }) {
                ZipEntry entry = zip.getEntry("payload/" + name);
                if (entry == null) throw new IOException("the download is missing " + name + ".");
                try (InputStream in = zip.getInputStream(entry)) {
                    replaceFile(in, new File(dllDirectory(), name));
                }
            }
        }
        Files.move(jar, stableJar().toPath(), StandardCopyOption.REPLACE_EXISTING, StandardCopyOption.ATOMIC_MOVE);
    }

    // --- network ---

    private static HttpURLConnection open(String url) throws IOException {
        HttpURLConnection connection = (HttpURLConnection) new URL(url).openConnection();
        connection.setRequestProperty("User-Agent", PROJECT_NAME + "-Updater");
        connection.setConnectTimeout(5000);
        connection.setReadTimeout(10000);
        return connection;
    }

    private static String httpGet(String url) throws IOException {
        HttpURLConnection connection = open(url);
        connection.setRequestProperty("Accept", "application/vnd.github+json");
        try (InputStream in = connection.getInputStream()) {
            return new String(readAll(in), StandardCharsets.UTF_8);
        } finally {
            connection.disconnect();
        }
    }

    private static void downloadWithProgress(String url, String version, Path target) throws IOException {
        final JDialog[] dialog = new JDialog[1];
        final JProgressBar[] progress = new JProgressBar[1];
        onEdt(() -> {
            JProgressBar bar = new JProgressBar(0, 100);
            progress[0] = bar;
            JPanel panel = new JPanel();
            panel.setLayout(new BoxLayout(panel, BoxLayout.Y_AXIS));
            panel.setBackground(BG);
            panel.setBorder(BorderFactory.createEmptyBorder(14, 16, 14, 16));
            JLabel label = new JLabel("Downloading " + PROJECT_NAME + " v" + version + "...");
            label.setAlignmentX(0f);
            bar.setAlignmentX(0f);
            panel.add(label);
            panel.add(Box.createVerticalStrut(10));
            panel.add(bar);
            dialog[0] = new JDialog((Frame) null, PROJECT_NAME + " — Update", false);
            dialog[0].setDefaultCloseOperation(WindowConstants.DO_NOTHING_ON_CLOSE);
            dialog[0].setContentPane(panel);
            dialog[0].setSize(380, 110);
            dialog[0].setResizable(false);
            dialog[0].setLocationRelativeTo(null);
            dialog[0].setAlwaysOnTop(true);
            dialog[0].setVisible(true);
        });
        final JProgressBar bar = progress[0];
        HttpURLConnection connection = open(url);
        try (InputStream in = connection.getInputStream(); OutputStream out = Files.newOutputStream(target)) {
            final long total = connection.getContentLengthLong();
            if (total <= 0) SwingUtilities.invokeLater(() -> bar.setIndeterminate(true));
            byte[] buffer = new byte[64 * 1024];
            long read = 0;
            int n;
            while ((n = in.read(buffer)) >= 0) {
                out.write(buffer, 0, n);
                read += n;
                if (read > MAX_DOWNLOAD_BYTES) throw new IOException("the download is too large.");
                if (total > 0) {
                    final int percent = (int) Math.min(100, read * 100 / total);
                    SwingUtilities.invokeLater(() -> bar.setValue(percent));
                }
            }
        } finally {
            connection.disconnect();
            onEdt(() -> dialog[0].dispose());
        }
    }

    private static String sha256(Path file) throws IOException {
        MessageDigest digest;
        try {
            digest = MessageDigest.getInstance("SHA-256");
        } catch (NoSuchAlgorithmException e) {
            throw new IOException(e);
        }
        try (InputStream in = Files.newInputStream(file)) {
            byte[] buffer = new byte[64 * 1024];
            int n;
            while ((n = in.read(buffer)) > 0) digest.update(buffer, 0, n);
        }
        StringBuilder hex = new StringBuilder();
        for (byte b : digest.digest()) hex.append(String.format("%02x", b));
        return hex.toString();
    }

    // --- shared with the installer ---

    static File configDirectory() {
        return new File(new File(System.getProperty("user.home"), ".config"), "toolscreen");
    }

    static File dllDirectory() {
        return new File(configDirectory(), "dlls");
    }

    /** The jar the pre-launch command runs. */
    static File stableJar() {
        return new File(configDirectory(), JAR_NAME);
    }

    /** The version in payload/version.txt, e.g. 1.5.2-macos.4. */
    static String readVersion() {
        try (InputStream in = Updater.class.getResourceAsStream("/payload/version.txt")) {
            return in == null ? "" : new String(readAll(in), StandardCharsets.UTF_8).trim();
        } catch (IOException e) {
            return "";
        }
    }

    /** Replaces the file in one step, so an error never leaves it half written. */
    static void replaceFile(InputStream in, File file) throws IOException {
        Files.createDirectories(file.getParentFile().toPath());
        Path temp = Files.createTempFile(file.getParentFile().toPath(), file.getName(), ".tmp");
        try {
            Files.copy(in, temp, StandardCopyOption.REPLACE_EXISTING);
            Files.move(temp, file.toPath(), StandardCopyOption.REPLACE_EXISTING, StandardCopyOption.ATOMIC_MOVE);
        } finally {
            Files.deleteIfExists(temp);
        }
    }

    /** Compares the numbers in two versions in order, so 1.5.2-macos.4 is older than 1.5.2-macos.10 and 1.5.3-macos.1. */
    static int compareVersions(String a, String b) {
        List<Integer> x = numbers(a);
        List<Integer> y = numbers(b);
        for (int i = 0; i < Math.max(x.size(), y.size()); i++) {
            int p = i < x.size() ? x.get(i) : 0;
            int q = i < y.size() ? y.get(i) : 0;
            if (p != q) return Integer.compare(p, q);
        }
        return 0;
    }

    private static List<Integer> numbers(String version) {
        List<Integer> result = new ArrayList<>();
        Matcher m = Pattern.compile("\\d+").matcher(version);
        while (m.find()) result.add(m.group().length() > 9 ? Integer.MAX_VALUE : Integer.parseInt(m.group()));
        return result;
    }

    static void applyDarkTheme() {
        Color btnBg = new Color(60, 60, 60);
        Font baseFont = new Font("SansSerif", Font.PLAIN, 13);
        Font btnFont = new Font("SansSerif", Font.PLAIN, 12);
        UIManager.put("OptionPane.background", BG);
        UIManager.put("OptionPane.messageForeground", FG);
        UIManager.put("OptionPane.messageFont", baseFont);
        UIManager.put("Panel.background", BG);
        UIManager.put("Panel.foreground", FG);
        UIManager.put("Label.background", BG);
        UIManager.put("Label.foreground", FG);
        UIManager.put("Label.font", baseFont);
        UIManager.put("Button.background", btnBg);
        UIManager.put("Button.foreground", FG);
        UIManager.put("Button.font", btnFont);
        UIManager.put("Button.border", BorderFactory.createCompoundBorder(
            BorderFactory.createLineBorder(new Color(80, 80, 80)), BorderFactory.createEmptyBorder(4, 12, 4, 12)));
    }

    static JButton styledButton(String text) {
        JButton btn = new JButton(text) {
            @Override
            protected void paintComponent(Graphics g) {
                Graphics2D g2 = (Graphics2D) g.create();
                g2.setRenderingHint(RenderingHints.KEY_ANTIALIASING, RenderingHints.VALUE_ANTIALIAS_ON);
                Container parent = getParent();
                g2.setColor(parent != null ? parent.getBackground() : BG);
                g2.fillRect(0, 0, getWidth(), getHeight());
                Color bg = getModel().isPressed() ? new Color(40, 40, 40)
                    : getModel().isRollover() ? new Color(80, 80, 80) : new Color(60, 60, 60);
                int w = Math.max(0, getWidth() - 2);
                int h = Math.max(0, getHeight() - 2);
                g2.setColor(bg);
                g2.fillRoundRect(1, 1, w, h, 10, 10);
                g2.setColor(new Color(100, 100, 100));
                if (w > 1 && h > 1) g2.drawRoundRect(1, 1, w - 1, h - 1, 10, 10);
                super.paintComponent(g2);
                g2.dispose();
            }
        };
        btn.setFont(new Font("SansSerif", Font.BOLD, 12));
        btn.setForeground(FG);
        btn.setCursor(new Cursor(Cursor.HAND_CURSOR));
        btn.setContentAreaFilled(false);
        btn.setFocusPainted(false);
        btn.setBorderPainted(false);
        btn.setOpaque(false);
        btn.setRolloverEnabled(true);
        btn.setBorder(BorderFactory.createEmptyBorder(8, 20, 8, 20));
        return btn;
    }

    static String escape(String s) {
        return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;");
    }

    // --- dialogs ---

    private static boolean confirmUpdate(String current, String latest) {
        final boolean[] accepted = { false };
        onEdt(() -> {
            String message = "<html><body style='width: 380px; font-family: sans-serif; color: #e0e0e0;'>"
                + "<p style='margin:0 0 10px 0; color: #4CAF50; font-size: 15px;'><b>Update available</b></p>"
                + "<p style='margin:0 0 10px 0;'>A newer version of <b>" + PROJECT_NAME + "</b> is available.</p>"
                + "<table style='margin:0 0 10px 0; font-size: 12px;'>"
                + row("Current", "v" + escape(current)) + row("Latest", "v" + escape(latest)) + row("Release page", RELEASES_PAGE)
                + "</table>"
                + "<p style='margin:0; color: #c7ced6; font-size: 11px;'>If you choose <b>Update</b>, the new version is downloaded"
                + " and used from this launch on.</p>"
                + "</body></html>";
            JButton skip = styledButton("Not now");
            JButton update = styledButton("Update");
            skip.addActionListener(e -> close(skip));
            update.addActionListener(e -> {
                accepted[0] = true;
                close(update);
            });
            showDialog(PROJECT_NAME + " — Update", new JLabel(message), new Object[] { skip, update }, update);
        });
        return accepted[0];
    }

    private static String row(String key, String value) {
        return "<tr><td style='padding:2px 10px 2px 0; color: #c7ced6;'>" + key + "</td><td style='color:#81D4FA;'><b>" + value
            + "</b></td></tr>";
    }

    private static void showMessage(String title, String html) {
        onEdt(() -> {
            JButton ok = styledButton("OK");
            ok.addActionListener(e -> close(ok));
            showDialog(title, new JLabel("<html><body style='width: 340px; font-family: sans-serif; color: #e0e0e0;'>" + html
                + "</body></html>"), new Object[] { ok }, ok);
        });
    }

    /** A modal dialog kept above the launcher, which stays the active app. */
    private static void showDialog(String title, Object message, Object[] options, Object initial) {
        JOptionPane pane = new JOptionPane(message, JOptionPane.PLAIN_MESSAGE, JOptionPane.DEFAULT_OPTION, null, options, initial);
        JDialog dialog = pane.createDialog(title);
        dialog.setAlwaysOnTop(true);
        dialog.setVisible(true);
        dialog.dispose();
    }

    private static void close(JButton button) {
        Window window = SwingUtilities.getWindowAncestor(button);
        if (window != null) window.dispose();
    }

    // --- helpers ---

    private static void onEdt(Runnable task) {
        if (SwingUtilities.isEventDispatchThread()) {
            task.run();
            return;
        }
        try {
            SwingUtilities.invokeAndWait(task);
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        } catch (InvocationTargetException e) {
            throw new IllegalStateException(e.getCause());
        }
    }

    private static byte[] readAll(InputStream in) throws IOException {
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        byte[] buffer = new byte[8192];
        int n;
        while ((n = in.read(buffer)) >= 0) {
            out.write(buffer, 0, n);
            if (out.size() > 4 * 1024 * 1024) throw new IOException("response too large");
        }
        return out.toByteArray();
    }

    private static String text(Object value) {
        return value instanceof String ? (String) value : "";
    }

    private static int javaVersion() {
        String v = System.getProperty("java.specification.version", "0");
        if (v.startsWith("1.")) v = v.substring(2);
        try {
            return Integer.parseInt(v);
        } catch (NumberFormatException e) {
            return 0;
        }
    }

    private static void log(String message) {
        System.out.println("[" + PROJECT_NAME + "] " + message);
    }

    /** Reads the GitHub API's JSON: objects become maps, arrays lists, numbers doubles. */
    static final class Json {
        private final String s;
        private int i;

        private Json(String s) {
            this.s = s;
        }

        static Object parse(String s) {
            Json json = new Json(s);
            Object value = json.value();
            json.skipWhitespace();
            if (json.i != s.length()) throw json.error();
            return value;
        }

        private Object value() {
            skipWhitespace();
            if (i >= s.length()) throw error();
            char c = s.charAt(i);
            if (c == '{') return object();
            if (c == '[') return array();
            if (c == '"') return string();
            if (s.startsWith("true", i)) {
                i += 4;
                return Boolean.TRUE;
            }
            if (s.startsWith("false", i)) {
                i += 5;
                return Boolean.FALSE;
            }
            if (s.startsWith("null", i)) {
                i += 4;
                return null;
            }
            return number();
        }

        private Map<String, Object> object() {
            Map<String, Object> map = new LinkedHashMap<>();
            i++;
            skipWhitespace();
            if (consume('}')) return map;
            do {
                skipWhitespace();
                String key = string();
                skipWhitespace();
                expect(':');
                map.put(key, value());
                skipWhitespace();
            } while (consume(','));
            expect('}');
            return map;
        }

        private List<Object> array() {
            List<Object> list = new ArrayList<>();
            i++;
            skipWhitespace();
            if (consume(']')) return list;
            do {
                list.add(value());
                skipWhitespace();
            } while (consume(','));
            expect(']');
            return list;
        }

        private String string() {
            expect('"');
            StringBuilder out = new StringBuilder();
            while (true) {
                char c = next();
                if (c == '"') return out.toString();
                if (c != '\\') {
                    out.append(c);
                    continue;
                }
                char e = next();
                switch (e) {
                case 'b': out.append('\b'); break;
                case 'f': out.append('\f'); break;
                case 'n': out.append('\n'); break;
                case 'r': out.append('\r'); break;
                case 't': out.append('\t'); break;
                case 'u':
                    if (i + 4 > s.length()) throw error();
                    try {
                        out.append((char) Integer.parseInt(s.substring(i, i + 4), 16));
                    } catch (NumberFormatException ex) {
                        throw error();
                    }
                    i += 4;
                    break;
                default: out.append(e);
                }
            }
        }

        private Double number() {
            int start = i;
            while (i < s.length() && "+-0123456789.eE".indexOf(s.charAt(i)) >= 0) i++;
            try {
                return Double.valueOf(s.substring(start, i));
            } catch (NumberFormatException e) {
                throw error();
            }
        }

        private char next() {
            if (i >= s.length()) throw error();
            return s.charAt(i++);
        }

        private boolean consume(char c) {
            if (i < s.length() && s.charAt(i) == c) {
                i++;
                return true;
            }
            return false;
        }

        private void expect(char c) {
            if (!consume(c)) throw error();
        }

        private void skipWhitespace() {
            while (i < s.length() && Character.isWhitespace(s.charAt(i))) i++;
        }

        private IllegalArgumentException error() {
            return new IllegalArgumentException("invalid JSON at " + i);
        }
    }
}
