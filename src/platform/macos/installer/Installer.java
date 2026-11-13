package com.toolscreen.macos;

import java.awt.BasicStroke;
import java.awt.BorderLayout;
import java.awt.Color;
import java.awt.Component;
import java.awt.Container;
import java.awt.Cursor;
import java.awt.Font;
import java.awt.Graphics;
import java.awt.Graphics2D;
import java.awt.RenderingHints;
import java.awt.Window;
import java.awt.geom.Path2D;
import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.net.URISyntaxException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardCopyOption;
import java.util.ArrayList;
import java.util.List;
import java.util.Optional;
import java.util.Properties;
import java.util.concurrent.TimeUnit;

import javax.swing.BorderFactory;
import javax.swing.Box;
import javax.swing.BoxLayout;
import javax.swing.Icon;
import javax.swing.JButton;
import javax.swing.JLabel;
import javax.swing.JOptionPane;
import javax.swing.JPanel;
import javax.swing.SwingUtilities;
import javax.swing.UIManager;

/**
 * Toolscreen installer for macOS (Prism Launcher / MultiMC).
 *
 * Copies Toolscreen.dylib and liblogger.dylib to ~/.config/toolscreen/dlls and
 * adds "-agentpath:<Toolscreen.dylib>" to the instance's JVM arguments, so the
 * game loads Toolscreen at startup. Run it again to uninstall.
 */
public final class Installer {
    private static final String PROJECT_NAME = "Toolscreen";
    private static final String AGENT_PREFIX = "-agentpath:";
    private static final String DYLIB_NAME = "Toolscreen.dylib";
    private static final String LOGGER_NAME = "liblogger.dylib";

    private static final Color BG = new Color(43, 43, 43);
    private static final Color FG = new Color(224, 224, 224);

    private Installer() {}

    public static void main(String[] args) {
        final String version = readVersion();
        if (!System.getProperty("os.name", "").toLowerCase().startsWith("mac")) {
            JOptionPane.showMessageDialog(null, "This installer is for macOS.", PROJECT_NAME, JOptionPane.ERROR_MESSAGE);
            return;
        }
        SwingUtilities.invokeLater(() -> run(version));
    }

    private static void run(String version) {
        applyDarkTheme();
        File instanceDir = instanceDirectory();
        File instanceCfg = instanceDir == null ? null : new File(instanceDir, "instance.cfg");
        if (instanceCfg == null || !instanceCfg.isFile()) {
            showSetupRequired(version);
            return;
        }

        try {
            List<String> lines = Files.readAllLines(instanceCfg.toPath(), StandardCharsets.UTF_8);
            File dllDir = dllDirectory();
            String agentArg = AGENT_PREFIX + new File(dllDir, DYLIB_NAME).getAbsolutePath();

            if (!ensureLauncherClosed(instanceDir)) return;

            extractPayload(dllDir);
            InstanceSettings settings = InstanceSettings.parse(lines);
            if (!settings.hasToolscreenAgent()) {
                saveRestoreState(instanceDir, settings);
                if (!settings.overrideJavaArgs()) {
                    // Overriding JVM arguments stops the global ones from applying,
                    // so carry them over into the instance.
                    settings.setJvmArgs(globalJvmArgs(instanceDir));
                    settings.setOverrideJavaArgs(true);
                }
            }
            settings.setToolscreenAgent(agentArg);
            Files.write(instanceCfg.toPath(), settings.lines(), StandardCharsets.UTF_8);
            reopenLauncher();

            showInstalled(version, instanceDir, () -> uninstall(instanceDir, instanceCfg));
        } catch (Exception e) {
            showError("Installation failed: " + e.getMessage());
        }
    }

    // --- install / uninstall -------------------------------------------------------------

    private static String uninstall(File instanceDir, File instanceCfg) {
        try {
            if (!ensureLauncherClosed(instanceDir)) return "Close the launcher first.";
            List<String> lines = Files.readAllLines(instanceCfg.toPath(), StandardCharsets.UTF_8);
            InstanceSettings settings = InstanceSettings.parse(lines);
            settings.removeToolscreenAgent();
            Properties state = loadRestoreState(instanceDir);
            if (state != null) {
                settings.setOverrideJavaArgs(Boolean.parseBoolean(state.getProperty("overrideJavaArgs", "false")));
                if (!Boolean.parseBoolean(state.getProperty("overrideJavaArgs", "false"))) {
                    settings.setJvmArgs(state.getProperty("jvmArgs", ""));
                }
                deleteRestoreState(instanceDir);
            }
            Files.write(instanceCfg.toPath(), settings.lines(), StandardCharsets.UTF_8);
            reopenLauncher();
            return null;
        } catch (Exception e) {
            return e.getMessage();
        }
    }

    private static void extractPayload(File dllDir) throws IOException {
        Files.createDirectories(dllDir.toPath());
        for (String name : new String[] { DYLIB_NAME, LOGGER_NAME }) {
            try (InputStream in = Installer.class.getResourceAsStream("/payload/" + name)) {
                if (in == null) throw new IOException("The installer is missing " + name + ".");
                Path target = new File(dllDir, name).toPath();
                Path temp = Files.createTempFile(dllDir.toPath(), name, ".tmp");
                Files.copy(in, temp, StandardCopyOption.REPLACE_EXISTING);
                Files.move(temp, target, StandardCopyOption.REPLACE_EXISTING, StandardCopyOption.ATOMIC_MOVE);
            }
        }
    }

    private static File dllDirectory() {
        return new File(new File(new File(System.getProperty("user.home"), ".config"), "toolscreen"), "dlls");
    }

    private static File instanceDirectory() {
        try {
            File jar = new File(Installer.class.getProtectionDomain().getCodeSource().getLocation().toURI());
            return jar.getParentFile();
        } catch (URISyntaxException | SecurityException e) {
            return null;
        }
    }

    /** JvmArgs from the launcher's global settings (two folders above the instance). */
    private static String globalJvmArgs(File instanceDir) {
        File dataDir = instanceDir.getParentFile() == null ? null : instanceDir.getParentFile().getParentFile();
        if (dataDir == null) return "";
        for (String name : new String[] { "prismlauncher.cfg", "multimc.cfg", "polymc.cfg" }) {
            File cfg = new File(dataDir, name);
            if (!cfg.isFile()) continue;
            try {
                InstanceSettings global = InstanceSettings.parse(Files.readAllLines(cfg.toPath(), StandardCharsets.UTF_8));
                return global.jvmArgs();
            } catch (IOException ignored) {
                return "";
            }
        }
        return "";
    }

    // --- restore state ---------------------------------------------------------------------

    private static File stateFile(File instanceDir) {
        String key = Integer.toHexString(instanceDir.getAbsolutePath().hashCode());
        return new File(dllDirectory().getParentFile(), "installer-" + key + ".properties");
    }

    private static void saveRestoreState(File instanceDir, InstanceSettings settings) throws IOException {
        Properties p = new Properties();
        p.setProperty("instance", instanceDir.getAbsolutePath());
        p.setProperty("overrideJavaArgs", Boolean.toString(settings.overrideJavaArgs()));
        p.setProperty("jvmArgs", settings.jvmArgs());
        File file = stateFile(instanceDir);
        Files.createDirectories(file.getParentFile().toPath());
        try (var out = Files.newOutputStream(file.toPath())) {
            p.store(out, "Toolscreen installer: launcher settings before installation");
        }
    }

    private static Properties loadRestoreState(File instanceDir) {
        File file = stateFile(instanceDir);
        if (!file.isFile()) return null;
        Properties p = new Properties();
        try (var in = Files.newInputStream(file.toPath())) {
            p.load(in);
            return p;
        } catch (IOException e) {
            return null;
        }
    }

    private static void deleteRestoreState(File instanceDir) {
        stateFile(instanceDir).delete();
    }

    // --- launcher restart ------------------------------------------------------------------

    private static String launcherAppToReopen;

    /**
     * Prism Launcher and MultiMC keep instance settings in memory and rewrite
     * instance.cfg, so they must not be running while it is edited.
     */
    private static boolean ensureLauncherClosed(File instanceDir) {
        Optional<ProcessHandle> launcher = findLauncherProcess();
        if (launcher.isEmpty()) return true;
        String appPath = appBundlePath(launcher.get());
        String appName = appPath == null ? "the launcher" : new File(appPath).getName().replace(".app", "");
        int choice = JOptionPane.showConfirmDialog(null,
            appName + " has to restart so it picks up the new settings.\nRestart it now?",
            PROJECT_NAME, JOptionPane.OK_CANCEL_OPTION, JOptionPane.QUESTION_MESSAGE);
        if (choice != JOptionPane.OK_OPTION) return false;
        try {
            if (appPath != null) {
                new ProcessBuilder("osascript", "-e", "quit app \"" + appPath + "\"").start().waitFor(10, TimeUnit.SECONDS);
            }
            try {
                launcher.get().onExit().get(15, TimeUnit.SECONDS);
            } catch (java.util.concurrent.TimeoutException timeout) {
                showError(appName + " did not close. Close it and run the installer again.");
                return false;
            }
        } catch (Exception e) {
            showError("Could not close " + appName + ": " + e.getMessage());
            return false;
        }
        launcherAppToReopen = appPath;
        return true;
    }

    private static void reopenLauncher() {
        if (launcherAppToReopen == null) return;
        try {
            new ProcessBuilder("open", launcherAppToReopen).start();
        } catch (IOException ignored) {
        }
        launcherAppToReopen = null;
    }

    private static Optional<ProcessHandle> findLauncherProcess() {
        return ProcessHandle.allProcesses().filter(p -> {
            String command = p.info().command().orElse("").toLowerCase();
            return command.contains("prismlauncher.app/") || command.contains("multimc.app/") || command.contains("polymc.app/");
        }).findFirst();
    }

    private static String appBundlePath(ProcessHandle process) {
        String command = process.info().command().orElse("");
        int end = command.indexOf(".app/");
        return end < 0 ? null : command.substring(0, end + 4);
    }

    // --- version ---------------------------------------------------------------------------

    private static String readVersion() {
        try (InputStream in = Installer.class.getResourceAsStream("/payload/version.txt")) {
            if (in == null) return "";
            return new String(in.readAllBytes(), StandardCharsets.UTF_8).trim();
        } catch (IOException e) {
            return "";
        }
    }

    // --- dialogs (same look as the Windows installer) -----------------------------------------

    private static void applyDarkTheme() {
        Color fieldBg = new Color(30, 30, 30);
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
        UIManager.put("TextField.background", fieldBg);
        UIManager.put("TextField.foreground", FG);
    }

    private static void showInstalled(String version, File instanceDir, java.util.function.Supplier<String> uninstaller) {
        JPanel panel = new JPanel();
        panel.setOpaque(true);
        panel.setBackground(BG);
        panel.setLayout(new BoxLayout(panel, BoxLayout.Y_AXIS));
        panel.setBorder(BorderFactory.createEmptyBorder(4, 4, 4, 4));

        JPanel header = new JPanel(new BorderLayout());
        header.setOpaque(true);
        header.setBackground(BG);
        header.setAlignmentX(Component.LEFT_ALIGNMENT);

        JLabel title = new JLabel("Installed Successfully!");
        title.setIcon(successIcon());
        title.setIconTextGap(8);
        title.setFont(new Font("SansSerif", Font.BOLD, 16));
        title.setForeground(new Color(76, 175, 80));
        header.add(title, BorderLayout.CENTER);

        JButton uninstall = styledButton("Uninstall");
        uninstall.setBorder(BorderFactory.createEmptyBorder(4, 12, 4, 12));
        uninstall.setFont(new Font("SansSerif", Font.BOLD, 11));
        uninstall.addActionListener(e -> {
            int choice = JOptionPane.showConfirmDialog(SwingUtilities.getWindowAncestor(uninstall),
                "Are you sure you want to undo the installation?\nThis will remove the launcher integration.",
                "Confirm Uninstall", JOptionPane.YES_NO_OPTION, JOptionPane.WARNING_MESSAGE);
            if (choice != JOptionPane.YES_OPTION) return;
            String error = uninstaller.get();
            if (error == null) {
                uninstall.setText("Uninstalled");
                uninstall.setEnabled(false);
                title.setIcon(null);
                title.setText("Uninstalled!");
                title.setForeground(FG);
            } else {
                uninstall.setText("Error");
                JOptionPane.showMessageDialog(SwingUtilities.getWindowAncestor(uninstall), "Failed: " + error, "Error",
                    JOptionPane.ERROR_MESSAGE);
            }
        });
        header.add(uninstall, BorderLayout.EAST);
        panel.add(header);
        panel.add(Box.createVerticalStrut(10));

        String message = "<html><body style='font-family: sans-serif; color: #e0e0e0;'>"
            + "<p style='margin:0 0 8px 0;'>" + PROJECT_NAME + " has been configured for this instance.</p>"
            + "<table style='margin:0 0 10px 0; color: #d9d9d9; font-size: 12px;'>"
            + "<tr><td style='padding:2px 10px 2px 0; color: #c7ced6;'>Instance</td><td style='color:#81D4FA;'><b>"
            + escape(instanceDir.getName()) + "</b></td></tr>"
            + "<tr><td style='padding:2px 10px 2px 0; color: #c7ced6;'>Path</td><td style='color: #c7ced6; font-size: 11px;'>"
            + escape(instanceDir.getAbsolutePath()) + "</td></tr>"
            + "</table>"
            + "<p style='margin:0; color: #c7ced6; font-size: 11px;'>You can now launch Minecraft from your launcher.</p>"
            + "</body></html>";
        JLabel body = new JLabel(message);
        body.setAlignmentX(Component.LEFT_ALIGNMENT);
        panel.add(body);

        JButton ok = styledButton("OK");
        ok.addActionListener(e -> {
            Window w = SwingUtilities.getWindowAncestor(ok);
            if (w != null) w.dispose();
        });
        JOptionPane.showOptionDialog(null, panel, PROJECT_NAME + (version.isEmpty() ? "" : " v" + version),
            JOptionPane.DEFAULT_OPTION, JOptionPane.PLAIN_MESSAGE, null, new Object[] { ok }, ok);
    }

    private static void showSetupRequired(String version) {
        String message = "<html><body style='width: 360px; font-family: sans-serif; color: #e0e0e0;'>"
            + "<p style='margin:0 0 10px 0; color: #FFA726; font-size: 15px;'><b>Setup Required</b></p>"
            + "<p style='margin:0 0 10px 0;'>To install " + PROJECT_NAME + ", follow these steps:</p>"
            + "<ol style='margin:0 0 0 0; padding-left: 20px; color: #ccc;'>"
            + "<li style='margin-bottom: 3px;'>Open your instance folder:"
            + "<ul style='margin-top:4px; margin-bottom: 0px; margin-left: 20px; color: #aaa; font-size: 11px;'>"
            + "<li><b style='color:#4CAF50;'>MultiMC:</b> Right-click instance &rarr; Instance Folder</li>"
            + "<li><b style='color:#42A5F5;'>Prism:</b> Right-click instance &rarr; Folder</li>"
            + "<li><b style='color:#9575CD;'>Other launchers:</b> Not currently supported</li>"
            + "</ul></li>"
            + "<li style='margin-bottom: 3px;'>Drop this JAR file into that folder.</li>"
            + "<li>Double-click the JAR in that folder to install.</li>"
            + "</ol></body></html>";
        JButton ok = styledButton("OK");
        ok.addActionListener(e -> {
            Window w = SwingUtilities.getWindowAncestor(ok);
            if (w != null) w.dispose();
        });
        JOptionPane.showOptionDialog(null, new JLabel(message),
            PROJECT_NAME + (version.isEmpty() ? "" : " v" + version) + " — Setup Required", JOptionPane.DEFAULT_OPTION,
            JOptionPane.PLAIN_MESSAGE, null, new Object[] { ok }, ok);
    }

    private static void showError(String text) {
        JOptionPane.showMessageDialog(null, text, PROJECT_NAME, JOptionPane.ERROR_MESSAGE);
    }

    private static String escape(String s) {
        return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;");
    }

    private static JButton styledButton(String text) {
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

    private static Icon successIcon() {
        return new Icon() {
            @Override
            public void paintIcon(Component c, Graphics g, int x, int y) {
                Graphics2D g2 = (Graphics2D) g.create();
                g2.setRenderingHint(RenderingHints.KEY_ANTIALIASING, RenderingHints.VALUE_ANTIALIAS_ON);
                g2.setColor(new Color(76, 175, 80));
                g2.fillOval(x, y, 16, 16);
                g2.setColor(Color.WHITE);
                g2.setStroke(new BasicStroke(2.0f, BasicStroke.CAP_ROUND, BasicStroke.JOIN_ROUND));
                Path2D.Float check = new Path2D.Float();
                check.moveTo(x + 4, y + 8);
                check.lineTo(x + 7, y + 11);
                check.lineTo(x + 12, y + 5);
                g2.draw(check);
                g2.dispose();
            }

            @Override
            public int getIconWidth() {
                return 16;
            }

            @Override
            public int getIconHeight() {
                return 16;
            }
        };
    }

    // --- instance.cfg (QSettings INI) ---------------------------------------------------------

    /** Section-aware edits of the [General] keys Prism and MultiMC read. */
    static final class InstanceSettings {
        private final List<String> lines;

        private InstanceSettings(List<String> lines) {
            this.lines = new ArrayList<>(lines);
        }

        static InstanceSettings parse(List<String> lines) {
            return new InstanceSettings(lines);
        }

        List<String> lines() {
            return lines;
        }

        boolean overrideJavaArgs() {
            return "true".equalsIgnoreCase(get("OverrideJavaArgs"));
        }

        void setOverrideJavaArgs(boolean value) {
            set("OverrideJavaArgs", Boolean.toString(value));
        }

        String jvmArgs() {
            return unquote(get("JvmArgs"));
        }

        void setJvmArgs(String value) {
            set("JvmArgs", quote(value));
        }

        boolean hasToolscreenAgent() {
            for (String arg : splitArgs(jvmArgs())) {
                if (isToolscreenAgent(arg)) return true;
            }
            return false;
        }

        void setToolscreenAgent(String agentArg) {
            List<String> args = new ArrayList<>();
            for (String arg : splitArgs(jvmArgs())) {
                if (!isToolscreenAgent(arg)) args.add(arg);
            }
            args.add(agentArg);
            setJvmArgs(joinArgs(args));
        }

        void removeToolscreenAgent() {
            List<String> args = new ArrayList<>();
            for (String arg : splitArgs(jvmArgs())) {
                if (!isToolscreenAgent(arg)) args.add(arg);
            }
            setJvmArgs(joinArgs(args));
        }

        private static boolean isToolscreenAgent(String arg) {
            return arg.startsWith(AGENT_PREFIX) && arg.endsWith("/" + DYLIB_NAME);
        }

        private int sectionStart(String name) {
            for (int i = 0; i < lines.size(); i++) {
                if (lines.get(i).trim().equals("[" + name + "]")) return i;
            }
            return -1;
        }

        private String get(String key) {
            boolean inGeneral = sectionStart("General") < 0;
            for (String line : lines) {
                String t = line.trim();
                if (t.startsWith("[") && t.endsWith("]")) {
                    inGeneral = t.equals("[General]");
                    continue;
                }
                int eq = t.indexOf('=');
                if (inGeneral && eq > 0 && t.substring(0, eq).trim().equals(key)) return t.substring(eq + 1);
            }
            return "";
        }

        private void set(String key, String value) {
            int general = sectionStart("General");
            boolean inGeneral = general < 0;
            for (int i = 0; i < lines.size(); i++) {
                String t = lines.get(i).trim();
                if (t.startsWith("[") && t.endsWith("]")) {
                    inGeneral = t.equals("[General]");
                    continue;
                }
                int eq = t.indexOf('=');
                if (inGeneral && eq > 0 && t.substring(0, eq).trim().equals(key)) {
                    lines.set(i, key + "=" + value);
                    return;
                }
            }
            if (general < 0) {
                lines.add(0, "[General]");
                general = 0;
            }
            lines.add(general + 1, key + "=" + value);
        }

        /** QSettings writes values containing special characters in double quotes. */
        private static String unquote(String raw) {
            String v = raw.trim();
            if (v.length() < 2 || !v.startsWith("\"") || !v.endsWith("\"")) return v;
            StringBuilder out = new StringBuilder();
            for (int i = 1; i < v.length() - 1; i++) {
                char c = v.charAt(i);
                if (c == '\\' && i + 1 < v.length() - 1) {
                    char n = v.charAt(++i);
                    out.append(n == 'n' ? '\n' : n == 't' ? '\t' : n);
                } else {
                    out.append(c);
                }
            }
            return out.toString();
        }

        private static String quote(String value) {
            return "\"" + value.replace("\\", "\\\\").replace("\"", "\\\"") + "\"";
        }

        private static List<String> splitArgs(String s) {
            List<String> out = new ArrayList<>();
            StringBuilder cur = new StringBuilder();
            boolean inQuotes = false;
            boolean any = false;
            for (int i = 0; i < s.length(); i++) {
                char c = s.charAt(i);
                if (c == '"') {
                    inQuotes = !inQuotes;
                    any = true;
                } else if (Character.isWhitespace(c) && !inQuotes) {
                    if (any) out.add(cur.toString());
                    cur.setLength(0);
                    any = false;
                } else {
                    cur.append(c);
                    any = true;
                }
            }
            if (any) out.add(cur.toString());
            return out;
        }

        private static String joinArgs(List<String> args) {
            StringBuilder out = new StringBuilder();
            for (String arg : args) {
                if (out.length() > 0) out.append(' ');
                out.append(arg.indexOf(' ') >= 0 ? "\"" + arg + "\"" : arg);
            }
            return out.toString();
        }
    }
}
