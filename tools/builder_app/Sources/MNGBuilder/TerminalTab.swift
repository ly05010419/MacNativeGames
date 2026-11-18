import AppKit
import SwiftTerm
import SwiftUI

/// The terminal page (user 2026-11-13: "在launcher 里 嵌入一个 终端 … 用户如何有问题 直接里面 启动 ai 然后 问 打包为什么失败了
/// ai自己就能看看 发生了什么 修复 然后继续"): the player's own shell in the checkout, kept for the Launcher's life, and two
/// buttons that type an AI's command into it with what failed. The AI works through tools/mng.py, the engine this window
/// uses (the same job lock, so the window shows its build). Nothing runs without the player pressing a button.
final class TerminalHost: NSObject, LocalProcessTerminalViewDelegate {
    static let shared = TerminalHost()
    let view = LocalProcessTerminalView(frame: NSRect(x: 0, y: 0, width: 900, height: 520))
    private var running = false

    override init() {
        super.init()
        view.processDelegate = self
    }

    /// The player's login shell in the checkout (else the home folder), once; again after it ended.
    func start() {
        guard !running else { return }
        running = true
        let shell = ProcessInfo.processInfo.environment["SHELL"] ?? "/bin/zsh"
        var env = Terminal.getEnvironmentVariables(termName: "xterm-256color")
        if let repo = Paths.repo { env.append("MNG_REPO=\(repo.path)") }
        let folder = Paths.repo?.path ?? FileManager.default.homeDirectoryForCurrentUser.path
        view.startProcess(executable: shell, args: ["-l"], environment: env,
                          execName: "-" + (shell as NSString).lastPathComponent, currentDirectory: folder)
    }

    /// Text typed into the shell as if from the keyboard (a command ends with a newline).
    func type(_ text: String) {
        start()
        view.send(data: ArraySlice(Array(text.utf8)))
    }

    /// The AI the shell is running now ("claude" or "codex", a child of the shell), or nil.
    func runningAI() -> String? {
        guard running, let pid = view.process?.shellPid, pid > 0 else { return nil }
        let ps = Process()
        ps.executableURL = URL(fileURLWithPath: "/bin/ps")
        ps.arguments = ["-Ao", "ppid=,args="]
        let pipe = Pipe()
        ps.standardOutput = pipe
        guard (try? ps.run()) != nil else { return nil }
        let out = String(decoding: pipe.fileHandleForReading.readDataToEndOfFile(), as: UTF8.self)
        ps.waitUntilExit()
        for line in out.split(separator: "\n") {
            let fields = line.trimmingCharacters(in: .whitespaces).split(separator: " ", maxSplits: 1)
            guard fields.count == 2, Int32(fields[0]) == pid else { continue }
            let args = fields[1].lowercased()
            if args.contains("claude") { return "claude" }
            if args.contains("codex") { return "codex" }
        }
        return nil
    }

    /// A request to the AI: said to the one already running in the terminal (the user's screenshot of 2026-11-15: the
    /// button typed a shell command into Claude's prompt), else `tool` started with it.
    func ask(_ tool: String, _ prompt: String) {
        if runningAI() != nil { type(prompt + "\r") } else { type(AIHelp.command(tool: tool, prompt: prompt)) }
    }

    func sizeChanged(source: LocalProcessTerminalView, newCols: Int, newRows: Int) {}
    func setTerminalTitle(source: LocalProcessTerminalView, title: String) {}
    func hostCurrentDirectoryUpdate(source: TerminalView, directory: String?) {}
    func processTerminated(source: TerminalView, exitCode: Int32?) { running = false }
}

/// What the AI is asked: what failed (the log of the job that stopped, when there is one) and how to work here
/// (user 2026-11-15: no sentence about keys or game files).
enum AIHelp {
    static func prompt(log: String?) -> String {
        let failed = log.map { T.s("A build or update in MNG Launcher failed; its log is \($0). ", "MNG Launcher 的打包或更新失败了，日志在 \($0)。") }
            ?? T.s("I am building a game with MNG Launcher and need help. ", "我在用 MNG Launcher 打包游戏，需要帮忙。")
        return failed + T.s(
            "First run python3 tools/mng.py status and python3 tools/mng.py check to see this Mac's state, read the log to find the cause, fix it following docs/install.md, then build or update again with python3 tools/mng.py (never by hand with build_app.py).",
            "先运行 python3 tools/mng.py status 和 python3 tools/mng.py check 看这台 Mac 的状态，读日志找出原因，按 docs/install.md 修好，再用 python3 tools/mng.py 重新打包或更新（不要手拼 build_app.py）。")
    }

    /// 提交 PR 给作者 (user 2026-11-15: "把最近修复的 bug 用pr的方法提交给我"): what was fixed on this Mac, as a pull request
    /// to the project, shown to the player before it is sent.
    static func prPrompt() -> String {
        T.s("Send the bug we fixed on this Mac to the author as a pull request: 1) look at git status and git diff and keep only the changes of the fix; 2) on a new branch from the newest origin/main, commit them with what went wrong, why, the fix and how it was tested; no personal information, whole logs, game files or keys; 3) open the pull request with gh on https://github.com/ly05010419/MacNativeGames (fork it first when this account cannot push there); 4) before sending, show me the title, the description and the list of changes, and send only when I say so.",
            "把我们在这台 Mac 上修好的 bug 用 PR 提交给作者：1）看 git status 和 git diff，只留下这次修复相关的改动；2）从最新的 origin/main 开新分支提交，写清现象、原因、修法和怎么测的；不要带个人信息、完整日志、游戏文件或密钥；3）用 gh 在 https://github.com/ly05010419/MacNativeGames 开 PR（这个账号没有推送权限就先 fork）；4）发之前把标题、说明和改动列表给我看，我同意了再发。")
    }

    /// The shell line that starts `tool` with the prompt, or says how to install it.
    static func command(tool: String, prompt: String) -> String {
        let quoted = "'" + prompt.replacingOccurrences(of: "'", with: "'\\''") + "'"
        let missing = T.s("\(tool) is not installed on this Mac", "这台 Mac 没有安装 \(tool)")
        return "if command -v \(tool) >/dev/null; then \(tool) \(quoted); else echo '\(missing)'; fi\n"
    }
}

struct TerminalPage: View {
    @EnvironmentObject var model: Model

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack {
                Button { model.page = "home" } label: { Label(T.backToGames, systemImage: "chevron.left") }
                Spacer()
                Button(T.askClaude) { TerminalHost.shared.ask("claude", AIHelp.prompt(log: model.failureLog)) }
                Button(T.askCodex) { TerminalHost.shared.ask("codex", AIHelp.prompt(log: model.failureLog)) }
                Button(T.sendPR) { TerminalHost.shared.ask("claude", AIHelp.prPrompt()) }.help(T.sendPRHelp)
            }
            Text(T.terminalIntro).font(.callout).foregroundStyle(.secondary)
            TerminalBox().frame(maxWidth: .infinity, maxHeight: .infinity)
        }
        .padding(20)
        .onAppear { TerminalHost.shared.start() }
    }
}

/// The one terminal view, shown again each time the page opens (the shell and its output stay).
struct TerminalBox: NSViewRepresentable {
    func makeNSView(context: Context) -> LocalProcessTerminalView { TerminalHost.shared.view }
    func updateNSView(_ view: LocalProcessTerminalView, context: Context) {}
}
