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
import java.io.OutputStream;
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
import java.util.concurrent.TimeoutException;
import java.util.function.Supplier;
import java.util.regex.Pattern;

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
 * sets the instance's wrapper command so the game starts with both libraries
 * injected through DYLD_INSERT_LIBRARIES, LibLogger first: the macOS
 * counterpart of the Windows DLL injector. Run it again to uninstall.
 */
public final class Installer {
    private static final String PROJECT_NAME = "Toolscreen";
    private static final String ENV_COMMAND = "/usr/bin/env";
    private static final String INSERT_VARIABLE = "DYLD_INSERT_LIBRARIES=";
    // The first macOS build (1.5.2-macos.1) loaded Toolscreen as a JVM agent;
    // installing or uninstalling removes that.
    private static final String LEGACY_AGENT_PREFIX = "-agentpath:";
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
            if (!ensureLauncherClosed(instanceDir)) return;

            File dllDir = dllDirectory();
            extractPayload(dllDir);

            InstanceSettings settings = new InstanceSettings(Files.readAllLines(instanceCfg.toPath(), StandardCharsets.UTF_8));
            undoLegacyAgentInstall(instanceDir, settings);
            if (!settings.hasToolscreenWrapper()) {
                saveRestoreState(instanceDir, settings);
                if (!settings.overrideCommands()) {
                    // Overriding custom commands stops the global ones from applying,
                    // so carry them over into the instance.
                    InstanceSettings global = globalSettings(instanceDir);
                    settings.setPreLaunchCommand(global.preLaunchCommand());
                    settings.setWrapperCommand(global.wrapperCommand());
                    settings.setPostExitCommand(global.postExitCommand());
                }
            }
            settings.setOverrideCommands(true);
            settings.setToolscreenWrapper(injectionCommand(dllDir));
            writeLines(instanceCfg, settings.lines());
        } catch (Exception e) {
            showError("Installation failed: " + e.getMessage());
            return;
        } finally {
            reopenLauncher();
        }
        showInstalled(version, instanceDir, () -> uninstall(instanceDir, instanceCfg));
    }

    private static String uninstall(File instanceDir, File instanceCfg) {
        try {
            if (!ensureLauncherClosed(instanceDir)) return "Close the launcher first.";
            InstanceSettings settings = new InstanceSettings(Files.readAllLines(instanceCfg.toPath(), StandardCharsets.UTF_8));
            undoLegacyAgentInstall(instanceDir, settings);
            settings.removeToolscreenWrapper();
            Properties state = loadRestoreState(instanceDir);
            if (state != null && state.getProperty("overrideCommands") != null) {
                boolean override = Boolean.parseBoolean(state.getProperty("overrideCommands"));
                settings.setOverrideCommands(override);
                if (!override) {
                    settings.setPreLaunchCommand(state.getProperty("preLaunchCommand", ""));
                    settings.setWrapperCommand(state.getProperty("wrapperCommand", ""));
                    settings.setPostExitCommand(state.getProperty("postExitCommand", ""));
                }
            }
            writeLines(instanceCfg, settings.lines());
            deleteRestoreState(instanceDir);
            return null;
        } catch (Exception e) {
            return e.getMessage();
        } finally {
            reopenLauncher();
        }
    }

    private static void extractPayload(File dllDir) throws IOException {
        Files.createDirectories(dllDir.toPath());
        for (String name : new String[] { DYLIB_NAME, LOGGER_NAME }) {
            try (InputStream in = Installer.class.getResourceAsStream("/payload/" + name)) {
                if (in == null) throw new IOException("The installer is missing " + name + ".");
                Path temp = Files.createTempFile(dllDir.toPath(), name, ".tmp");
                try {
                    Files.copy(in, temp, StandardCopyOption.REPLACE_EXISTING);
                    Files.move(temp, new File(dllDir, name).toPath(), StandardCopyOption.REPLACE_EXISTING, StandardCopyOption.ATOMIC_MOVE);
                } finally {
                    Files.deleteIfExists(temp);
                }
            }
        }
    }

    /** Replaces the file in one step, so an error never leaves it half written. */
    private static void writeLines(File file, List<String> lines) throws IOException {
        Path temp = Files.createTempFile(file.getParentFile().toPath(), file.getName(), ".tmp");
        try {
            Files.write(temp, lines, StandardCharsets.UTF_8);
            Files.move(temp, file.toPath(), StandardCopyOption.REPLACE_EXISTING, StandardCopyOption.ATOMIC_MOVE);
        } finally {
            Files.deleteIfExists(temp);
        }
    }

    private static File dllDirectory() {
        return new File(new File(new File(System.getProperty("user.home"), ".config"), "toolscreen"), "dlls");
    }

    /** "/usr/bin/env DYLD_INSERT_LIBRARIES=<liblogger>:<Toolscreen>". */
    private static String injectionCommand(File dllDir) {
        String variable = INSERT_VARIABLE + new File(dllDir, LOGGER_NAME).getAbsolutePath() + ":"
            + new File(dllDir, DYLIB_NAME).getAbsolutePath();
        return ENV_COMMAND + " " + (variable.indexOf(' ') >= 0 ? "\"" + variable + "\"" : variable);
    }

    /** Removes the JVM agent the first macOS build added and restores the Java arguments it changed. */
    private static void undoLegacyAgentInstall(File instanceDir, InstanceSettings settings) {
        if (!settings.removeLegacyToolscreenAgent()) return;
        Properties state = loadRestoreState(instanceDir);
        if (state != null && state.getProperty("overrideJavaArgs") != null) {
            boolean override = Boolean.parseBoolean(state.getProperty("overrideJavaArgs"));
            settings.setOverrideJavaArgs(override);
            if (!override) settings.setJvmArgs(state.getProperty("jvmArgs", ""));
            deleteRestoreState(instanceDir);
        }
    }

    private static File instanceDirectory() {
        try {
            File jar = new File(Installer.class.getProtectionDomain().getCodeSource().getLocation().toURI());
            return jar.getParentFile();
        } catch (URISyntaxException | SecurityException e) {
            return null;
        }
    }

    /** The launcher's global settings (two folders above the instance); empty if not found. */
    private static InstanceSettings globalSettings(File instanceDir) {
        File dataDir = instanceDir.getParentFile() == null ? null : instanceDir.getParentFile().getParentFile();
        if (dataDir != null) {
            for (String name : new String[] { "prismlauncher.cfg", "multimc.cfg", "polymc.cfg" }) {
                File cfg = new File(dataDir, name);
                if (!cfg.isFile()) continue;
                try {
                    return new InstanceSettings(Files.readAllLines(cfg.toPath(), StandardCharsets.UTF_8));
                } catch (IOException ignored) {
                    break;
                }
            }
        }
        return new InstanceSettings(List.of());
    }

    private static File stateFile(File instanceDir) {
        String key = Integer.toHexString(instanceDir.getAbsolutePath().hashCode());
        return new File(dllDirectory().getParentFile(), "installer-" + key + ".properties");
    }

    private static void saveRestoreState(File instanceDir, InstanceSettings settings) throws IOException {
        Properties p = new Properties();
        p.setProperty("instance", instanceDir.getAbsolutePath());
        p.setProperty("overrideCommands", Boolean.toString(settings.overrideCommands()));
        p.setProperty("preLaunchCommand", settings.preLaunchCommand());
        p.setProperty("wrapperCommand", settings.wrapperCommand());
        p.setProperty("postExitCommand", settings.postExitCommand());
        File file = stateFile(instanceDir);
        Files.createDirectories(file.getParentFile().toPath());
        try (OutputStream out = Files.newOutputStream(file.toPath())) {
            p.store(out, "Toolscreen installer: launcher settings before installation");
        }
    }

    private static Properties loadRestoreState(File instanceDir) {
        File file = stateFile(instanceDir);
        if (!file.isFile()) return null;
        Properties p = new Properties();
        try (InputStream in = Files.newInputStream(file.toPath())) {
            p.load(in);
        } catch (IOException e) {
            return null;
        }
        return instanceDir.getAbsolutePath().equals(p.getProperty("instance")) ? p : null;
    }

    private static void deleteRestoreState(File instanceDir) {
        stateFile(instanceDir).delete();
    }

    private static String launcherAppToReopen;

    /**
     * Prism Launcher and MultiMC keep instance settings in memory and rewrite
     * instance.cfg, so they must not be running while it is edited.
     */
    private static boolean ensureLauncherClosed(File instanceDir) {
        Optional<ProcessHandle> launcher = findLauncherProcess();
        if (launcher.isEmpty()) return true;
        String appPath = appBundlePath(launcher.get());
        String appName = new File(appPath).getName().replaceFirst("(?i)\\.app$", "");
        int choice = JOptionPane.showConfirmDialog(null,
            appName + " has to restart so it picks up the new settings.\nRestart it now?",
            PROJECT_NAME, JOptionPane.OK_CANCEL_OPTION, JOptionPane.QUESTION_MESSAGE);
        if (choice != JOptionPane.OK_OPTION) return false;
        try {
            new ProcessBuilder("osascript", "-e", "on run argv", "-e", "tell application (item 1 of argv) to quit", "-e", "end run",
                appPath).start().waitFor(10, TimeUnit.SECONDS);
            try {
                launcher.get().onExit().get(15, TimeUnit.SECONDS);
            } catch (TimeoutException timeout) {
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

    /** Matches the executable inside the app bundle, e.g. "Prism Launcher.app/Contents/MacOS/prismlauncher". */
    private static Optional<ProcessHandle> findLauncherProcess() {
        return ProcessHandle.allProcesses().filter(p -> {
            String command = p.info().command().orElse("").toLowerCase();
            return command.endsWith(".app/contents/macos/prismlauncher") || command.endsWith(".app/contents/macos/multimc")
                || command.endsWith(".app/contents/macos/polymc");
        }).findFirst();
    }

    private static String appBundlePath(ProcessHandle process) {
        String command = process.info().command().orElse("");
        return command.substring(0, command.toLowerCase().indexOf(".app/") + 4);
    }

    private static String readVersion() {
        try (InputStream in = Installer.class.getResourceAsStream("/payload/version.txt")) {
            if (in == null) return "";
            return new String(in.readAllBytes(), StandardCharsets.UTF_8).trim();
        } catch (IOException e) {
            return "";
        }
    }

    private static void applyDarkTheme() {
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

    private static void showInstalled(String version, File instanceDir, Supplier<String> uninstaller) {
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

    /**
     * Section-aware edits of the [General] keys Prism and MultiMC read. Prism
     * writes instance.cfg with QSettings (a [General] section, quotes where
     * needed); MultiMC writes plain key=value lines and keeps quotes literally.
     */
    static final class InstanceSettings {
        // What injectionCommand writes, quoted or not.
        private static final Pattern TOOLSCREEN_WRAPPER = Pattern.compile("\\s*" + Pattern.quote(ENV_COMMAND) + "\\s+(\""
            + Pattern.quote(INSERT_VARIABLE) + "[^\"]*/" + Pattern.quote(DYLIB_NAME) + "\"|" + Pattern.quote(INSERT_VARIABLE) + "\\S*/"
            + Pattern.quote(DYLIB_NAME) + ")");
        private static final Pattern LEGACY_AGENT = Pattern.compile("\\s*(\"" + Pattern.quote(LEGACY_AGENT_PREFIX) + "[^\"]*/"
            + Pattern.quote(DYLIB_NAME) + "\"|" + Pattern.quote(LEGACY_AGENT_PREFIX) + "\\S*/" + Pattern.quote(DYLIB_NAME) + ")");

        private final List<String> lines;

        InstanceSettings(List<String> lines) {
            this.lines = new ArrayList<>(lines);
        }

        List<String> lines() {
            return lines;
        }

        boolean overrideCommands() {
            return "true".equalsIgnoreCase(decode(get("OverrideCommands")));
        }

        void setOverrideCommands(boolean value) {
            set("OverrideCommands", Boolean.toString(value));
        }

        String preLaunchCommand() {
            return decode(get("PreLaunchCommand"));
        }

        void setPreLaunchCommand(String value) {
            set("PreLaunchCommand", encode(value));
        }

        String wrapperCommand() {
            return decode(get("WrapperCommand"));
        }

        void setWrapperCommand(String value) {
            set("WrapperCommand", encode(value));
        }

        String postExitCommand() {
            return decode(get("PostExitCommand"));
        }

        void setPostExitCommand(String value) {
            set("PostExitCommand", encode(value));
        }

        void setOverrideJavaArgs(boolean value) {
            set("OverrideJavaArgs", Boolean.toString(value));
        }

        String jvmArgs() {
            return decode(get("JvmArgs"));
        }

        void setJvmArgs(String value) {
            set("JvmArgs", encode(value));
        }

        boolean hasToolscreenWrapper() {
            return TOOLSCREEN_WRAPPER.matcher(wrapperCommand()).find();
        }

        /** Appends the injection after any wrapper the user already has, replacing an older one. */
        void setToolscreenWrapper(String injection) {
            String rest = TOOLSCREEN_WRAPPER.matcher(wrapperCommand()).replaceAll("").trim();
            setWrapperCommand(rest.isEmpty() ? injection : rest + " " + injection);
        }

        void removeToolscreenWrapper() {
            if (hasToolscreenWrapper()) setWrapperCommand(TOOLSCREEN_WRAPPER.matcher(wrapperCommand()).replaceAll("").trim());
        }

        /** Returns true if a JVM agent from the first macOS build was removed. */
        boolean removeLegacyToolscreenAgent() {
            String args = jvmArgs();
            if (!LEGACY_AGENT.matcher(args).find()) return false;
            setJvmArgs(LEGACY_AGENT.matcher(args).replaceAll("").trim());
            return true;
        }

        private boolean isQSettingsFormat() {
            return sectionStart("General") >= 0;
        }

        private String encode(String value) {
            String escaped = value.replace("\\", "\\\\").replace("\n", "\\n").replace("\t", "\\t");
            if (!isQSettingsFormat()) return escaped.replace("#", "\\#");
            escaped = escaped.replace("\r", "\\r").replace("\"", "\\\"");
            boolean needsQuotes = value.indexOf(',') >= 0 || value.indexOf(';') >= 0 || value.indexOf('=') >= 0
                || (!value.isEmpty() && (Character.isWhitespace(value.charAt(0)) || Character.isWhitespace(value.charAt(value.length() - 1))));
            return needsQuotes ? "\"" + escaped + "\"" : escaped;
        }

        /** Reverses encode, and the quoting QSettings writes. */
        private String decode(String raw) {
            String v = raw.trim();
            if (isQSettingsFormat() && v.length() >= 2 && v.startsWith("\"") && v.endsWith("\"")) v = v.substring(1, v.length() - 1);
            StringBuilder out = new StringBuilder();
            for (int i = 0; i < v.length(); i++) {
                char c = v.charAt(i);
                if (c == '\\' && i + 1 < v.length()) {
                    char n = v.charAt(++i);
                    out.append(n == 'n' ? '\n' : n == 't' ? '\t' : n == 'r' ? '\r' : n);
                } else {
                    out.append(c);
                }
            }
            return out.toString();
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
            if (general >= 0) {
                lines.add(general + 1, key + "=" + value);
            } else if (lines.stream().anyMatch(l -> l.trim().startsWith("["))) {
                // Sections but no [General]: top-level keys belong in [General].
                lines.add(0, key + "=" + value);
                lines.add(0, "[General]");
            } else {
                lines.add(key + "=" + value);
            }
        }
    }
}
