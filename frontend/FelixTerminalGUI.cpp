#include <wx/wx.h>
#include <wx/notebook.h>
#include <wx/spinctrl.h>
#include <wx/statline.h>
#include <wx/listctrl.h>
#include <wx/textfile.h>
#include <wx/stdpaths.h>
#include <wx/dir.h>
#include <wx/filefn.h>
#include <wx/fileconf.h>
#include <wx/mstream.h>
#include <wx/filename.h>
#ifdef _WIN32
#include <wx/msw/registry.h>
#endif
#include "favicon.h"
#include <sstream>
#include <vector>

#ifdef __WXMSW__
#define FLT_EXECUTABLE "flt.exe"
#else
#define FLT_EXECUTABLE "./flt"
#endif

// ── Argument quoting ─────────────────────────────────────────────────────────
// Values typed by the user (key paths, custom shells, SSH commands, web roots)
// often contain spaces — e.g. "C:\Users\Jane Doe\.ssh\id_ed25519" or
// "C:\Program Files\Git\bin\bash.exe". Each one must reach flt as a single
// argument, so arguments are kept as a list and quoted only when displayed or
// (on Windows) when joined into the command line CreateProcess expects.

// Windows: quote per the MSVCRT/CommandLineToArgvW rules, so flt's argv sees
// exactly the original string (backslashes are literal unless they precede ").
static wxString WinQuoteArg(const wxString &a) {
    if (!a.empty() && a.find_first_of(" \t\n\v\"") == wxString::npos)
        return a;
    wxString out = "\"";
    size_t i = 0;
    const size_t n = a.length();
    while (true) {
        size_t backslashes = 0;
        while (i < n && a[i] == '\\') { ++i; ++backslashes; }
        if (i == n) {                       // double trailing backslashes before closing quote
            out.Append('\\', backslashes * 2);
            break;
        }
        if (a[i] == '"') {                  // escape the backslashes and the quote
            out.Append('\\', backslashes * 2 + 1);
            out += '"';
        } else {
            out.Append('\\', backslashes);
            out += a[i];
        }
        ++i;
    }
    out += "\"";
    return out;
}

// POSIX shells: single-quote anything that isn't plainly safe (display only —
// on Linux/macOS the arguments are passed to flt as an array, not a string).
static wxString PosixQuoteArg(const wxString &a) {
    if (!a.empty() && a.find_first_of(" \t\n'\"\\$`!*?[]{}()<>|&;#~") == wxString::npos)
        return a;
    wxString out = "'";
    for (wxUniChar c : a) {
        if (c == '\'') out += "'\\''";
        else            out += c;
    }
    out += "'";
    return out;
}

static wxString QuoteArgForDisplay(const wxString &a) {
#ifdef __WXMSW__
    return WinQuoteArg(a);
#else
    return PosixQuoteArg(a);
#endif
}

struct SessionConfig {
    wxString name;
    int connType = 0;
    wxString localShell;
    wxString termType = "xterm-256color";  // --term value, used for Local and SSH
    
    // Telnet
    wxString telnetHost;
    int telnetPort = 23;
    wxString telnetTType = "xterm-256color";
    bool telnetRaw = false;
    bool telnetSSL = false;
    
    // Serial
    wxString serialPort;
    int serialBaud = 9600;
    
    // SSH
    wxString sshUser;
    wxString sshHost = "localhost";
    int sshPort = 22;
    int sshAuthMethod = 0;
    wxString sshKeyPath;
    wxString sshKnownHosts;
    bool sshX11 = true;
    wxString sshCommand;  // Command to execute via SSH
    std::vector<wxString> localPF;
    std::vector<wxString> remotePF;
    std::vector<wxString> socks;
    
    // Web Server
    bool webServerEnabled = false;
    wxString webServerAddr = "127.0.0.1";
    int webServerPort = 53716;
    wxString webRootDir = "/";
};

enum {
    ID_MENU_EXPORT = wxID_HIGHEST + 1,
    ID_MENU_IMPORT
};

class FelixTerminalFrame : public wxFrame {
public:
    FelixTerminalFrame() : wxFrame(nullptr, wxID_ANY, "Felix Terminal", 
                                   wxDefaultPosition, wxSize(950, 650)) {
        
        // Set the icon from favicon data
        wxMemoryInputStream istream(favicon_ico, favicon_ico_len);
        wxImage img(istream, wxBITMAP_TYPE_ICO);
        if (img.IsOk()) {
            wxBitmap bmp(img);
            wxIcon icon;
            icon.CopyFromBitmap(bmp);
            SetIcon(icon);
        }
        
        // ========== MENU BAR ==========
        wxMenuBar *menuBar = new wxMenuBar();

        wxMenu *fileMenu = new wxMenu();
        fileMenu->Append(wxID_OPEN, "&Load Session\tCtrl+L");
        fileMenu->Append(wxID_SAVE, "&Save Session\tCtrl+S");
        fileMenu->Append(wxID_DELETE, "&Delete Session");
        fileMenu->AppendSeparator();
        fileMenu->Append(ID_MENU_EXPORT, "&Export Settings...\tCtrl+E");
        fileMenu->Append(ID_MENU_IMPORT, "&Import Settings...\tCtrl+I");
        fileMenu->AppendSeparator();
        fileMenu->Append(wxID_CLOSE, "&Close\tCtrl+Q");
        menuBar->Append(fileMenu, "&File");

        wxMenu *helpMenu = new wxMenu();
        helpMenu->Append(wxID_ABOUT, "&About Felix Terminal");
        menuBar->Append(helpMenu, "&Help");

        SetMenuBar(menuBar);

        fileMenu->Bind(wxEVT_MENU, &FelixTerminalFrame::OnLoadSession, this, wxID_OPEN);
        fileMenu->Bind(wxEVT_MENU, &FelixTerminalFrame::OnSaveSession, this, wxID_SAVE);
        fileMenu->Bind(wxEVT_MENU, &FelixTerminalFrame::OnDeleteSession, this, wxID_DELETE);
        fileMenu->Bind(wxEVT_MENU, &FelixTerminalFrame::OnExportSettings, this, ID_MENU_EXPORT);
        fileMenu->Bind(wxEVT_MENU, &FelixTerminalFrame::OnImportSettings, this, ID_MENU_IMPORT);
        fileMenu->Bind(wxEVT_MENU, &FelixTerminalFrame::OnClose, this, wxID_CLOSE);
        helpMenu->Bind(wxEVT_MENU, &FelixTerminalFrame::OnAbout, this, wxID_ABOUT);
        
        wxPanel *mainPanel = new wxPanel(this);
        wxBoxSizer *mainSizer = new wxBoxSizer(wxHORIZONTAL);

        // ========== LEFT PANEL: Session List ==========
        wxBoxSizer *leftSizer = new wxBoxSizer(wxVERTICAL);
        leftSizer->Add(new wxStaticText(mainPanel, wxID_ANY, "Saved Sessions"), 0, wxALL, 8);
        
        m_sessionList = new wxListCtrl(mainPanel, wxID_ANY, wxDefaultPosition, wxSize(200, 400), wxLC_REPORT | wxLC_SINGLE_SEL);
        m_sessionList->AppendColumn("Session");
        m_sessionList->SetColumnWidth(0, 180);
        m_sessionList->Bind(wxEVT_LIST_ITEM_ACTIVATED, &FelixTerminalFrame::OnLoadSession, this);
        leftSizer->Add(m_sessionList, 1, wxEXPAND | wxALL, 8);

        wxBoxSizer *btnSizer = new wxBoxSizer(wxVERTICAL);
        m_loadBtn = new wxButton(mainPanel, wxID_ANY, "Load");
        m_saveBtn = new wxButton(mainPanel, wxID_ANY, "Save");
        m_deleteBtn = new wxButton(mainPanel, wxID_ANY, "Delete");
        
        m_loadBtn->Bind(wxEVT_BUTTON, &FelixTerminalFrame::OnLoadSession, this);
        m_saveBtn->Bind(wxEVT_BUTTON, &FelixTerminalFrame::OnSaveSession, this);
        m_deleteBtn->Bind(wxEVT_BUTTON, &FelixTerminalFrame::OnDeleteSession, this);
        
        btnSizer->Add(m_loadBtn, 0, wxEXPAND | wxBOTTOM, 4);
        btnSizer->Add(m_saveBtn, 0, wxEXPAND | wxBOTTOM, 4);
        btnSizer->Add(m_deleteBtn, 0, wxEXPAND);
        leftSizer->Add(btnSizer, 0, wxEXPAND | wxALL, 8);

        mainSizer->Add(leftSizer, 0, wxEXPAND | wxALL, 0);
        mainSizer->Add(new wxStaticLine(mainPanel, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxLI_VERTICAL), 0, wxEXPAND);

        // ========== RIGHT PANEL: Settings ==========
        wxBoxSizer *rightSizer = new wxBoxSizer(wxVERTICAL);

        wxStaticBoxSizer *sessionNameBox = new wxStaticBoxSizer(wxHORIZONTAL, mainPanel, "Session Name");
        m_sessionNameCtrl = new wxTextCtrl(mainPanel, wxID_ANY, "New Session");
        sessionNameBox->Add(m_sessionNameCtrl, 1, wxEXPAND | wxALL, 4);
        rightSizer->Add(sessionNameBox, 0, wxEXPAND | wxALL, 8);

        wxStaticBoxSizer *termTypeBox = new wxStaticBoxSizer(wxHORIZONTAL, mainPanel, "Terminal Type (--term)");
        wxArrayString termTypeChoices;
        termTypeChoices.Add("xterm-256color");
        termTypeChoices.Add("xterm-kitty");
        termTypeChoices.Add("xterm");
        termTypeChoices.Add("screen-256color");
        termTypeChoices.Add("vt100");
        m_termTypeCtrl = new wxComboBox(mainPanel, wxID_ANY, "xterm-256color", wxDefaultPosition, wxDefaultSize, termTypeChoices);
        m_termTypeCtrl->SetToolTip("xterm-kitty requires the 'kitty' terminfo entry to be installed on the remote/local system (e.g. 'kitty-terminfo' package)");
        m_termTypeCtrl->Bind(wxEVT_TEXT, &FelixTerminalFrame::OnUpdatePreview, this);
        m_termTypeCtrl->Bind(wxEVT_COMBOBOX, &FelixTerminalFrame::OnUpdatePreview, this);
        termTypeBox->Add(m_termTypeCtrl, 1, wxEXPAND | wxALL, 4);
        rightSizer->Add(termTypeBox, 0, wxEXPAND | wxALL, 8);

        // Notebook for settings
        wxNotebook *notebook = new wxNotebook(mainPanel, wxID_ANY);

        // ---- Connection Tab (for type selection) ----
        CreateConnectionPanel(notebook, mainPanel);
        // ---- Local Shell Tab ----
        CreateLocalShellTab(notebook, mainPanel);
        // ---- Telnet Tab ----
        CreateTelnetTab(notebook, mainPanel);
        // ---- Serial Tab ----
        CreateSerialTab(notebook, mainPanel);
        // ---- SSH Tab ----
        CreateSSHTab(notebook, mainPanel);
        // ---- Port Forward Tab ----
        CreatePortForwardPanel(notebook, mainPanel);
        // ---- Web Server Tab ----
        CreateWebServerPanel(notebook, mainPanel);

        rightSizer->Add(notebook, 1, wxEXPAND | wxALL, 8);

        // ---- Command Preview ----
        wxStaticBoxSizer *cmdSizer = new wxStaticBoxSizer(wxVERTICAL, mainPanel, "Command Line");
        m_cmdPreview = new wxTextCtrl(mainPanel, wxID_ANY, "", wxDefaultPosition, wxSize(-1, 50), 
                                       wxTE_MULTILINE | wxTE_READONLY | wxTE_WORDWRAP);
        cmdSizer->Add(m_cmdPreview, 1, wxEXPAND | wxALL, 4);
        rightSizer->Add(cmdSizer, 0, wxEXPAND | wxALL, 8);

        // ---- Buttons ----
        wxBoxSizer *bottomBtnSizer = new wxBoxSizer(wxHORIZONTAL);
        wxButton *clearBtn = new wxButton(mainPanel, wxID_ANY, "Clear");
        m_openBtn = new wxButton(mainPanel, wxID_ANY, "Open");
        m_closeBtn = new wxButton(mainPanel, wxID_CLOSE, "Close");
        
        clearBtn->Bind(wxEVT_BUTTON, &FelixTerminalFrame::OnClear, this);
        m_openBtn->Bind(wxEVT_BUTTON, &FelixTerminalFrame::OnOpen, this);
        m_closeBtn->Bind(wxEVT_BUTTON, &FelixTerminalFrame::OnClose, this);

        bottomBtnSizer->Add(clearBtn, 0, wxRIGHT, 4);
        bottomBtnSizer->AddStretchSpacer();
        bottomBtnSizer->Add(m_openBtn, 0, wxRIGHT, 4);
        bottomBtnSizer->Add(m_closeBtn, 0);
        rightSizer->Add(bottomBtnSizer, 0, wxEXPAND | wxALL, 8);

        mainSizer->Add(rightSizer, 1, wxEXPAND | wxALL, 0);
        mainPanel->SetSizer(mainSizer);

        LoadSessions();
        UpdatePreview();
    }

private:
    wxListCtrl *m_sessionList;
    wxButton *m_loadBtn;
    wxButton *m_saveBtn;
    wxButton *m_deleteBtn;
    wxButton *m_openBtn;
    wxButton *m_closeBtn;
    wxTextCtrl *m_sessionNameCtrl;
    wxComboBox *m_termTypeCtrl;
    wxTextCtrl *m_cmdPreview;

    // Connection
    wxChoice *m_connTypeChoice;
    wxChoice *m_localShellChoice;
    wxTextCtrl *m_localShellCustomCtrl;
    
    // Telnet
    wxTextCtrl *m_telnetHostCtrl;
    wxSpinCtrl *m_telnetPortSpin;
    wxCheckBox *m_telnetRawCheck;
    wxCheckBox *m_telnetSSLCheck;
    
    // Serial
    wxTextCtrl *m_serialPortCtrl;
    wxChoice *m_serialBaudChoice;
    
    // SSH
    wxTextCtrl *m_sshUserCtrl;
    wxTextCtrl *m_sshHostCtrl;
    wxSpinCtrl *m_sshPortSpin;
    wxChoice *m_sshAuthChoice;
    wxTextCtrl *m_sshKeyCtrl;
    wxButton *m_browseSshKeyBtn;
    wxCheckBox *m_sshX11Check;
    wxComboBox *m_sshCommandCtrl;  // remote shell / command (-c); empty = login shell

    // Port Forwarding
    wxListBox *m_localPFList;
    wxListBox *m_remotePFList;
    wxListBox *m_socksList;
    std::vector<wxString> m_localPFEntries;
    std::vector<wxString> m_remotePFEntries;
    std::vector<wxString> m_socksEntries;

    // Web Server
    wxCheckBox *m_webServerEnabledCheck;
    wxTextCtrl *m_webServerAddrCtrl;
    wxSpinCtrl *m_webServerPortSpin;
    wxTextCtrl *m_webRootDirCtrl;
    wxButton *m_browseWebRootBtn;

    void CreateConnectionPanel(wxNotebook *notebook, wxPanel *mainPanel) {
        wxPanel *panel = new wxPanel(notebook);
        wxBoxSizer *sizer = new wxBoxSizer(wxVERTICAL);
        
        wxStaticBoxSizer *typeBox = new wxStaticBoxSizer(wxVERTICAL, panel, "Connection Type");
        
        wxBoxSizer *typeSizer = new wxBoxSizer(wxHORIZONTAL);
        typeSizer->Add(new wxStaticText(panel, wxID_ANY, "Type:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
        
        m_connTypeChoice = new wxChoice(panel, wxID_ANY);
        m_connTypeChoice->Append("Local Shell");
        m_connTypeChoice->Append("Telnet");
        m_connTypeChoice->Append("Serial");
        m_connTypeChoice->Append("SSH");
        m_connTypeChoice->SetSelection(0);
        m_connTypeChoice->Bind(wxEVT_CHOICE, &FelixTerminalFrame::OnConnTypeChange, this);
        
        typeSizer->Add(m_connTypeChoice, 1, wxEXPAND);
        typeBox->Add(typeSizer, 0, wxEXPAND | wxALL, 8);
        sizer->Add(typeBox, 0, wxEXPAND | wxALL, 8);
        
        panel->SetSizer(sizer);
        notebook->AddPage(panel, "Connection");
    }

    void CreateLocalShellTab(wxNotebook *notebook, wxPanel *mainPanel) {
        wxPanel *panel = new wxPanel(notebook);
        wxBoxSizer *sizer = new wxBoxSizer(wxVERTICAL);
        
        wxStaticBoxSizer *shellBox = new wxStaticBoxSizer(wxVERTICAL, panel, "Local Shell");
        wxBoxSizer *shellSizer = new wxBoxSizer(wxHORIZONTAL);
        shellSizer->Add(new wxStaticText(panel, wxID_ANY, "Shell:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
        
        m_localShellChoice = new wxChoice(panel, wxID_ANY);
        
        #ifdef __WXMSW__
        // Windows shells
        m_localShellChoice->Append("cmd (Default)");
        m_localShellChoice->Append("PowerShell");
        m_localShellChoice->Append("Custom");
        m_localShellChoice->SetSelection(0);
        #else
        // Linux/Unix shells
        m_localShellChoice->Append("/bin/bash (Default)");
        m_localShellChoice->Append("/bin/sh");
        m_localShellChoice->Append("/bin/zsh");
        m_localShellChoice->Append("Custom");
        m_localShellChoice->SetSelection(0);
        #endif
        
        m_localShellChoice->Bind(wxEVT_CHOICE, &FelixTerminalFrame::OnLocalShellChange, this);
        shellSizer->Add(m_localShellChoice, 1, wxEXPAND);
        shellBox->Add(shellSizer, 0, wxEXPAND | wxALL, 8);
        
        wxBoxSizer *customSizer = new wxBoxSizer(wxHORIZONTAL);
        customSizer->Add(new wxStaticText(panel, wxID_ANY, "Custom:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
        m_localShellCustomCtrl = new wxTextCtrl(panel, wxID_ANY, "");
        m_localShellCustomCtrl->Bind(wxEVT_TEXT, &FelixTerminalFrame::OnUpdatePreview, this);
        customSizer->Add(m_localShellCustomCtrl, 1, wxEXPAND);
        shellBox->Add(customSizer, 0, wxEXPAND | wxALL, 8);
        
        sizer->Add(shellBox, 0, wxEXPAND | wxALL, 8);
        panel->SetSizer(sizer);
        notebook->AddPage(panel, "Local Shell");
    }

    void CreateTelnetTab(wxNotebook *notebook, wxPanel *mainPanel) {
        wxPanel *panel = new wxPanel(notebook);
        wxBoxSizer *sizer = new wxBoxSizer(wxVERTICAL);
        
        wxStaticBoxSizer *telnetBox = new wxStaticBoxSizer(wxVERTICAL, panel, "Telnet Settings");
        
        wxBoxSizer *hostSizer = new wxBoxSizer(wxHORIZONTAL);
        hostSizer->Add(new wxStaticText(panel, wxID_ANY, "Host:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
        m_telnetHostCtrl = new wxTextCtrl(panel, wxID_ANY, "localhost");
        m_telnetHostCtrl->Bind(wxEVT_TEXT, &FelixTerminalFrame::OnUpdatePreview, this);
        hostSizer->Add(m_telnetHostCtrl, 1, wxEXPAND);
        telnetBox->Add(hostSizer, 0, wxEXPAND | wxALL, 8);
        
        wxBoxSizer *portSizer = new wxBoxSizer(wxHORIZONTAL);
        portSizer->Add(new wxStaticText(panel, wxID_ANY, "Port:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
        m_telnetPortSpin = new wxSpinCtrl(panel, wxID_ANY, "23", wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 1, 65535, 23);
        m_telnetPortSpin->Bind(wxEVT_SPINCTRL, &FelixTerminalFrame::OnUpdatePreview, this);
        portSizer->Add(m_telnetPortSpin, 1, wxEXPAND);
        telnetBox->Add(portSizer, 0, wxEXPAND | wxALL, 8);
        
        m_telnetRawCheck = new wxCheckBox(panel, wxID_ANY, "Raw Mode");
        m_telnetRawCheck->Bind(wxEVT_CHECKBOX, &FelixTerminalFrame::OnUpdatePreview, this);
        telnetBox->Add(m_telnetRawCheck, 0, wxALL, 8);
        
        m_telnetSSLCheck = new wxCheckBox(panel, wxID_ANY, "Use SSL/TLS");
        m_telnetSSLCheck->Bind(wxEVT_CHECKBOX, &FelixTerminalFrame::OnUpdatePreview, this);
        telnetBox->Add(m_telnetSSLCheck, 0, wxALL, 8);
        
        sizer->Add(telnetBox, 0, wxEXPAND | wxALL, 8);
        panel->SetSizer(sizer);
        notebook->AddPage(panel, "Telnet");
    }

    void CreateSerialTab(wxNotebook *notebook, wxPanel *mainPanel) {
        wxPanel *panel = new wxPanel(notebook);
        wxBoxSizer *sizer = new wxBoxSizer(wxVERTICAL);
        
        wxStaticBoxSizer *serialBox = new wxStaticBoxSizer(wxVERTICAL, panel, "Serial Settings");
        
        wxBoxSizer *portSizer = new wxBoxSizer(wxHORIZONTAL);
        portSizer->Add(new wxStaticText(panel, wxID_ANY, "Port:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
        
        #ifdef __WXMSW__
        wxString defaultPort = "COM1";
        #else
        wxString defaultPort = "/dev/ttyS0";
        #endif
        
        m_serialPortCtrl = new wxTextCtrl(panel, wxID_ANY, defaultPort);
        m_serialPortCtrl->Bind(wxEVT_TEXT, &FelixTerminalFrame::OnUpdatePreview, this);
        portSizer->Add(m_serialPortCtrl, 1, wxEXPAND);
        serialBox->Add(portSizer, 0, wxEXPAND | wxALL, 8);
        
        wxBoxSizer *baudSizer = new wxBoxSizer(wxHORIZONTAL);
        baudSizer->Add(new wxStaticText(panel, wxID_ANY, "Baud Rate:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
        m_serialBaudChoice = new wxChoice(panel, wxID_ANY);
        m_serialBaudChoice->Append("1200");
        m_serialBaudChoice->Append("2400");
        m_serialBaudChoice->Append("4800");
        m_serialBaudChoice->Append("9600");
        m_serialBaudChoice->Append("19200");
        m_serialBaudChoice->Append("38400");
        m_serialBaudChoice->Append("57600");
        m_serialBaudChoice->Append("115200");
        m_serialBaudChoice->SetSelection(3);  // Default 9600
        m_serialBaudChoice->Bind(wxEVT_CHOICE, &FelixTerminalFrame::OnUpdatePreview, this);
        baudSizer->Add(m_serialBaudChoice, 1, wxEXPAND);
        serialBox->Add(baudSizer, 0, wxEXPAND | wxALL, 8);
        
        sizer->Add(serialBox, 0, wxEXPAND | wxALL, 8);
        panel->SetSizer(sizer);
        notebook->AddPage(panel, "Serial");
    }

    void CreateSSHTab(wxNotebook *notebook, wxPanel *mainPanel) {
        wxScrolledWindow *scrollPanel = new wxScrolledWindow(notebook);
        scrollPanel->SetScrollRate(5, 5);
        wxBoxSizer *sizer = new wxBoxSizer(wxVERTICAL);
        
        wxStaticBoxSizer *sshBox = new wxStaticBoxSizer(wxVERTICAL, scrollPanel, "SSH Settings");
        
        wxBoxSizer *userSizer = new wxBoxSizer(wxHORIZONTAL);
        userSizer->Add(new wxStaticText(scrollPanel, wxID_ANY, "User:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
        m_sshUserCtrl = new wxTextCtrl(scrollPanel, wxID_ANY, wxGetenv("USER"));
        m_sshUserCtrl->Bind(wxEVT_TEXT, &FelixTerminalFrame::OnUpdatePreview, this);
        userSizer->Add(m_sshUserCtrl, 1, wxEXPAND);
        sshBox->Add(userSizer, 0, wxEXPAND | wxALL, 8);
        
        wxBoxSizer *hostSizer = new wxBoxSizer(wxHORIZONTAL);
        hostSizer->Add(new wxStaticText(scrollPanel, wxID_ANY, "Host:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
        m_sshHostCtrl = new wxTextCtrl(scrollPanel, wxID_ANY, "localhost");
        m_sshHostCtrl->Bind(wxEVT_TEXT, &FelixTerminalFrame::OnUpdatePreview, this);
        hostSizer->Add(m_sshHostCtrl, 1, wxEXPAND);
        sshBox->Add(hostSizer, 0, wxEXPAND | wxALL, 8);
        
        wxBoxSizer *portSizer = new wxBoxSizer(wxHORIZONTAL);
        portSizer->Add(new wxStaticText(scrollPanel, wxID_ANY, "Port:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
        m_sshPortSpin = new wxSpinCtrl(scrollPanel, wxID_ANY, "22", wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 1, 65535, 22);
        m_sshPortSpin->Bind(wxEVT_SPINCTRL, &FelixTerminalFrame::OnUpdatePreview, this);
        portSizer->Add(m_sshPortSpin, 1, wxEXPAND);
        sshBox->Add(portSizer, 0, wxEXPAND | wxALL, 8);
        
        wxBoxSizer *authSizer = new wxBoxSizer(wxHORIZONTAL);
        authSizer->Add(new wxStaticText(scrollPanel, wxID_ANY, "Auth Method:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
        m_sshAuthChoice = new wxChoice(scrollPanel, wxID_ANY);
        m_sshAuthChoice->Append("Password");
        m_sshAuthChoice->Append("Public Key");
        m_sshAuthChoice->SetSelection(0);
        m_sshAuthChoice->Bind(wxEVT_CHOICE, &FelixTerminalFrame::OnSSHAuthChange, this);
        authSizer->Add(m_sshAuthChoice, 1, wxEXPAND);
        sshBox->Add(authSizer, 0, wxEXPAND | wxALL, 8);
        
        wxBoxSizer *keySizer = new wxBoxSizer(wxHORIZONTAL);
        keySizer->Add(new wxStaticText(scrollPanel, wxID_ANY, "Key Path:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
        m_sshKeyCtrl = new wxTextCtrl(scrollPanel, wxID_ANY, "");
        m_sshKeyCtrl->Bind(wxEVT_TEXT, &FelixTerminalFrame::OnUpdatePreview, this);
        keySizer->Add(m_sshKeyCtrl, 1, wxEXPAND | wxRIGHT, 4);
        m_browseSshKeyBtn = new wxButton(scrollPanel, wxID_ANY, "Browse");
        m_browseSshKeyBtn->Bind(wxEVT_BUTTON, &FelixTerminalFrame::OnBrowseSshKey, this);
        keySizer->Add(m_browseSshKeyBtn, 0, wxEXPAND);
        sshBox->Add(keySizer, 0, wxEXPAND | wxALL, 8);
        
        m_sshX11Check = new wxCheckBox(scrollPanel, wxID_ANY, "X11 Forwarding");
        m_sshX11Check->SetValue(true);
        m_sshX11Check->Bind(wxEVT_CHECKBOX, &FelixTerminalFrame::OnUpdatePreview, this);
        sshBox->Add(m_sshX11Check, 0, wxALL, 8);
        
        wxBoxSizer *cmdSizer = new wxBoxSizer(wxHORIZONTAL);
        cmdSizer->Add(new wxStaticText(scrollPanel, wxID_ANY, "Remote shell:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
        // Editable: pick a preset or type any command. Runs with a terminal
        // attached (flt -c), so shells and full-screen programs are interactive.
        wxArrayString remoteShells;
        remoteShells.Add("/bin/sh");
        remoteShells.Add("/bin/bash");
        remoteShells.Add("/bin/zsh");
        remoteShells.Add("bash --norc --noprofile");
        remoteShells.Add("tmux new -A -s main");
        remoteShells.Add("screen -xR");
        m_sshCommandCtrl = new wxComboBox(scrollPanel, wxID_ANY, "", wxDefaultPosition, wxDefaultSize,
                                          remoteShells, wxCB_DROPDOWN);
        m_sshCommandCtrl->SetHint("Default login shell");
        m_sshCommandCtrl->SetToolTip("Shell or command to run on the server instead of your login shell.\n"
                                     "Leave empty for the default. Examples: /bin/sh, bash --norc,\n"
                                     "tmux new -A -s main (reattach to a persistent session).");
        m_sshCommandCtrl->Bind(wxEVT_TEXT, &FelixTerminalFrame::OnUpdatePreview, this);
        m_sshCommandCtrl->Bind(wxEVT_COMBOBOX, &FelixTerminalFrame::OnUpdatePreview, this);
        cmdSizer->Add(m_sshCommandCtrl, 1, wxEXPAND);
        sshBox->Add(cmdSizer, 0, wxEXPAND | wxALL, 8);
        
        sizer->Add(sshBox, 0, wxEXPAND | wxALL, 8);
        scrollPanel->SetSizer(sizer);
        notebook->AddPage(scrollPanel, "SSH");
    }

    void CreatePortForwardPanel(wxNotebook *notebook, wxPanel *mainPanel) {
        wxPanel *panel = new wxPanel(notebook);
        wxBoxSizer *sizer = new wxBoxSizer(wxHORIZONTAL);
        
        // Local Port Forward
        wxBoxSizer *localSizer = new wxBoxSizer(wxVERTICAL);
        localSizer->Add(new wxStaticText(panel, wxID_ANY, "Local Port Forward (L)"), 0, wxALL, 8);
        m_localPFList = new wxListBox(panel, wxID_ANY);
        localSizer->Add(m_localPFList, 1, wxEXPAND | wxALL, 8);
        
        wxBoxSizer *localBtnSizer = new wxBoxSizer(wxHORIZONTAL);
        wxButton *localAddBtn = new wxButton(panel, wxID_ANY, "Add");
        wxButton *localRemoveBtn = new wxButton(panel, wxID_ANY, "Remove");
        localAddBtn->Bind(wxEVT_BUTTON, &FelixTerminalFrame::OnAddLocalPF, this);
        localRemoveBtn->Bind(wxEVT_BUTTON, &FelixTerminalFrame::OnRemoveLocalPF, this);
        localBtnSizer->Add(localAddBtn, 0, wxRIGHT, 4);
        localBtnSizer->Add(localRemoveBtn, 0);
        localSizer->Add(localBtnSizer, 0, wxEXPAND | wxALL, 8);
        
        // Remote Port Forward
        wxBoxSizer *remoteSizer = new wxBoxSizer(wxVERTICAL);
        remoteSizer->Add(new wxStaticText(panel, wxID_ANY, "Remote Port Forward (R)"), 0, wxALL, 8);
        m_remotePFList = new wxListBox(panel, wxID_ANY);
        remoteSizer->Add(m_remotePFList, 1, wxEXPAND | wxALL, 8);
        
        wxBoxSizer *remoteBtnSizer = new wxBoxSizer(wxHORIZONTAL);
        wxButton *remoteAddBtn = new wxButton(panel, wxID_ANY, "Add");
        wxButton *remoteRemoveBtn = new wxButton(panel, wxID_ANY, "Remove");
        remoteAddBtn->Bind(wxEVT_BUTTON, &FelixTerminalFrame::OnAddRemotePF, this);
        remoteRemoveBtn->Bind(wxEVT_BUTTON, &FelixTerminalFrame::OnRemoveRemotePF, this);
        remoteBtnSizer->Add(remoteAddBtn, 0, wxRIGHT, 4);
        remoteBtnSizer->Add(remoteRemoveBtn, 0);
        remoteSizer->Add(remoteBtnSizer, 0, wxEXPAND | wxALL, 8);
        
        // SOCKS Forward
        wxBoxSizer *socksSizer = new wxBoxSizer(wxVERTICAL);
        socksSizer->Add(new wxStaticText(panel, wxID_ANY, "SOCKS Forward (D)"), 0, wxALL, 8);
        m_socksList = new wxListBox(panel, wxID_ANY);
        socksSizer->Add(m_socksList, 1, wxEXPAND | wxALL, 8);
        
        wxBoxSizer *socksBtnSizer = new wxBoxSizer(wxHORIZONTAL);
        wxButton *socksAddBtn = new wxButton(panel, wxID_ANY, "Add");
        wxButton *socksRemoveBtn = new wxButton(panel, wxID_ANY, "Remove");
        socksAddBtn->Bind(wxEVT_BUTTON, &FelixTerminalFrame::OnAddSOCKS, this);
        socksRemoveBtn->Bind(wxEVT_BUTTON, &FelixTerminalFrame::OnRemoveSOCKS, this);
        socksBtnSizer->Add(socksAddBtn, 0, wxRIGHT, 4);
        socksBtnSizer->Add(socksRemoveBtn, 0);
        socksSizer->Add(socksBtnSizer, 0, wxEXPAND | wxALL, 8);
        
        sizer->Add(localSizer, 1, wxEXPAND | wxALL, 0);
        sizer->Add(remoteSizer, 1, wxEXPAND | wxALL, 0);
        sizer->Add(socksSizer, 1, wxEXPAND | wxALL, 0);
        
        panel->SetSizer(sizer);
        notebook->AddPage(panel, "Port Forward");
    }

    void CreateWebServerPanel(wxNotebook *notebook, wxPanel *mainPanel) {
        wxPanel *panel = new wxPanel(notebook);
        wxBoxSizer *sizer = new wxBoxSizer(wxVERTICAL);
        
        wxStaticBoxSizer *webBox = new wxStaticBoxSizer(wxVERTICAL, panel, "Web Server Settings");
        
        m_webServerEnabledCheck = new wxCheckBox(panel, wxID_ANY, "Enable Web Server");
        m_webServerEnabledCheck->Bind(wxEVT_CHECKBOX, &FelixTerminalFrame::OnUpdatePreview, this);
        webBox->Add(m_webServerEnabledCheck, 0, wxALL, 8);
        
        wxBoxSizer *addrSizer = new wxBoxSizer(wxHORIZONTAL);
        addrSizer->Add(new wxStaticText(panel, wxID_ANY, "Listen Address:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
        m_webServerAddrCtrl = new wxTextCtrl(panel, wxID_ANY, "127.0.0.1");
        m_webServerAddrCtrl->Bind(wxEVT_TEXT, &FelixTerminalFrame::OnUpdatePreview, this);
        addrSizer->Add(m_webServerAddrCtrl, 1, wxEXPAND);
        webBox->Add(addrSizer, 0, wxEXPAND | wxALL, 8);
        
        wxBoxSizer *portSizer = new wxBoxSizer(wxHORIZONTAL);
        portSizer->Add(new wxStaticText(panel, wxID_ANY, "Port:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
        m_webServerPortSpin = new wxSpinCtrl(panel, wxID_ANY, "53716", wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 1, 65535, 53716);
        m_webServerPortSpin->Bind(wxEVT_SPINCTRL, &FelixTerminalFrame::OnUpdatePreview, this);
        portSizer->Add(m_webServerPortSpin, 1, wxEXPAND);
        webBox->Add(portSizer, 0, wxEXPAND | wxALL, 8);
        
        wxBoxSizer *dirSizer = new wxBoxSizer(wxHORIZONTAL);
        dirSizer->Add(new wxStaticText(panel, wxID_ANY, "Root Directory:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
        m_webRootDirCtrl = new wxTextCtrl(panel, wxID_ANY, "/");
        m_webRootDirCtrl->Bind(wxEVT_TEXT, &FelixTerminalFrame::OnUpdatePreview, this);
        dirSizer->Add(m_webRootDirCtrl, 1, wxEXPAND | wxRIGHT, 4);
        m_browseWebRootBtn = new wxButton(panel, wxID_ANY, "Browse");
        m_browseWebRootBtn->Bind(wxEVT_BUTTON, &FelixTerminalFrame::OnBrowseWebRoot, this);
        dirSizer->Add(m_browseWebRootBtn, 0, wxEXPAND);
        webBox->Add(dirSizer, 0, wxEXPAND | wxALL, 8);
        
        sizer->Add(webBox, 0, wxEXPAND | wxALL, 8);
        panel->SetSizer(sizer);
        notebook->AddPage(panel, "Web Server");
    }

    void OnConnTypeChange(wxCommandEvent &event) {
        UpdatePreview();
    }

    void OnLocalShellChange(wxCommandEvent &event) {
        int selection = m_localShellChoice->GetSelection();
        bool isCustom = (selection == m_localShellChoice->GetCount() - 1);
        m_localShellCustomCtrl->Enable(isCustom);
        UpdatePreview();
    }

    void OnSSHAuthChange(wxCommandEvent &event) {
        int selection = m_sshAuthChoice->GetSelection();
        m_sshKeyCtrl->Enable(selection == 1);  // Enable key path only for public key auth
        m_browseSshKeyBtn->Enable(selection == 1);
        UpdatePreview();
    }

    void OnUpdatePreview(wxCommandEvent &event) {
        UpdatePreview();
    }

    void OnBrowseSshKey(wxCommandEvent &event) {
        wxString sshDir = wxGetHomeDir() + "/.ssh";
        wxFileDialog dlg(this, "Select SSH private key file", sshDir, "", 
                        "SSH Keys (id_*)|id_*|All files (*.*)|*.*|All files (*)|*", 
                        wxFD_OPEN | wxFD_FILE_MUST_EXIST);
        if (dlg.ShowModal() == wxID_OK) {
            m_sshKeyCtrl->SetValue(dlg.GetPath());
            UpdatePreview();
        }
    }

    void OnBrowseWebRoot(wxCommandEvent &event) {
        wxDirDialog dlg(this, "Select Web Root Directory");
        if (dlg.ShowModal() == wxID_OK) {
            m_webRootDirCtrl->SetValue(dlg.GetPath());
            UpdatePreview();
        }
    }

    void OnAddLocalPF(wxCommandEvent &event) {
        wxTextEntryDialog dlg(this, "Enter local port forward (e.g., 8080:remote:80):");
        if (dlg.ShowModal() == wxID_OK) {
            wxString entry = dlg.GetValue();
            if (!entry.empty()) {
                m_localPFList->Append(entry);
                m_localPFEntries.push_back(entry);
            }
        }
        UpdatePreview();
    }

    void OnRemoveLocalPF(wxCommandEvent &event) {
        int sel = m_localPFList->GetSelection();
        if (sel != wxNOT_FOUND) {
            m_localPFList->Delete(sel);
            m_localPFEntries.erase(m_localPFEntries.begin() + sel);
        }
        UpdatePreview();
    }

    void OnAddRemotePF(wxCommandEvent &event) {
        wxTextEntryDialog dlg(this, "Enter remote port forward (e.g., 8080:local:80):");
        if (dlg.ShowModal() == wxID_OK) {
            wxString entry = dlg.GetValue();
            if (!entry.empty()) {
                m_remotePFList->Append(entry);
                m_remotePFEntries.push_back(entry);
            }
        }
        UpdatePreview();
    }

    void OnRemoveRemotePF(wxCommandEvent &event) {
        int sel = m_remotePFList->GetSelection();
        if (sel != wxNOT_FOUND) {
            m_remotePFList->Delete(sel);
            m_remotePFEntries.erase(m_remotePFEntries.begin() + sel);
        }
        UpdatePreview();
    }

    void OnAddSOCKS(wxCommandEvent &event) {
        wxTextEntryDialog dlg(this, "Enter SOCKS forward (e.g., 1080):");
        if (dlg.ShowModal() == wxID_OK) {
            wxString entry = dlg.GetValue();
            if (!entry.empty()) {
                m_socksList->Append(entry);
                m_socksEntries.push_back(entry);
            }
        }
        UpdatePreview();
    }

    void OnRemoveSOCKS(wxCommandEvent &event) {
        int sel = m_socksList->GetSelection();
        if (sel != wxNOT_FOUND) {
            m_socksList->Delete(sel);
            m_socksEntries.erase(m_socksEntries.begin() + sel);
        }
        UpdatePreview();
    }

    // Build flt's arguments (without the executable) from the current UI state.
    std::vector<wxString> BuildArgs() {
        std::vector<wxString> args;
        auto add = [&](const wxString &a) { args.push_back(a); };

        int connType = m_connTypeChoice->GetSelection();

        switch (connType) {
            case 0: { // Local Shell
                wxString shell;
                int selection = m_localShellChoice->GetSelection();
                if (selection == (int)m_localShellChoice->GetCount() - 1) {
                    // Custom option is the last one. Passed through as ONE argument:
                    // on Windows flt hands it to CreateProcess as a full command line,
                    // so it may contain arguments ("wsl -d Ubuntu") or a quoted path
                    // with spaces:  "C:\Program Files\Git\bin\bash.exe" --login
                    shell = m_localShellCustomCtrl->GetValue();
                    shell.Trim(true).Trim(false);
                } else {
                    // Extract the shell path from the choice string (everything before the space/paren)
                    wxString choice = m_localShellChoice->GetStringSelection();
                    int spacePos = choice.Find(' ');
                    shell = (spacePos != wxNOT_FOUND) ? choice.Left(spacePos) : choice;
                }
                // flt takes the shell as a plain positional argument
                if (!shell.empty()) add(shell);
                if (!m_termTypeCtrl->GetValue().empty()) { add("--term"); add(m_termTypeCtrl->GetValue()); }
                break;
            }
            case 1: // Telnet
                add("--telnet");
                add(m_telnetHostCtrl->GetValue() + ":" + wxString::Format("%d", m_telnetPortSpin->GetValue()));
                if (m_telnetRawCheck->GetValue()) add("--raw");
                if (m_telnetSSLCheck->GetValue()) add("--ssl");
                break;
            case 2: // Serial
                add("--serial");      add(m_serialPortCtrl->GetValue());
                add("--serial-baud"); add(m_serialBaudChoice->GetStringSelection());
                break;
            case 3: // SSH
                add("--ssh");
                {
                    // No user -> leave "user@" off so flt prompts for it
                    wxString user = m_sshUserCtrl->GetValue();
                    user.Trim(true).Trim(false);
                    add((user.empty() ? wxString() : user + "@") + m_sshHostCtrl->GetValue() + ":" +
                        wxString::Format("%d", m_sshPortSpin->GetValue()));
                }
                if (m_sshAuthChoice->GetSelection() == 1 && !m_sshKeyCtrl->GetValue().empty()) {
                    add("-i"); add(m_sshKeyCtrl->GetValue());
                }
                if (!m_sshX11Check->GetValue()) add("--no-x11");
                if (!m_sshCommandCtrl->GetValue().empty()) {
                    add("-c"); add(m_sshCommandCtrl->GetValue());   // whole command = one argument
                }
                if (!m_termTypeCtrl->GetValue().empty()) { add("--term"); add(m_termTypeCtrl->GetValue()); }

                // Port forwarding
                for (const auto &pf : m_localPFEntries)  { add("-L"); add(pf); }
                for (const auto &pf : m_remotePFEntries) { add("-R"); add(pf); }
                for (const auto &sock : m_socksEntries)  { add("-D"); add(sock); }
                break;
        }

        // Web server
        if (m_webServerEnabledCheck->GetValue()) {
            add("--webserver");
            add(m_webServerAddrCtrl->GetValue() + ":" + wxString::Format("%d", m_webServerPortSpin->GetValue()));
            if (m_webRootDirCtrl->GetValue() != "/") { add("--web-root"); add(m_webRootDirCtrl->GetValue()); }
        }
        return args;
    }

    void UpdatePreview() {
        wxString cmd = FLT_EXECUTABLE;
        for (const wxString &a : BuildArgs())
            cmd += " " + QuoteArgForDisplay(a);
        m_cmdPreview->SetValue(cmd);
    }

    SessionConfig GetCurrentConfig(const wxString &name) {
        SessionConfig cfg;
        cfg.name = name;
        cfg.connType = m_connTypeChoice->GetSelection();
        
        // Extract shell from choice
        int selection = m_localShellChoice->GetSelection();
        if (selection == m_localShellChoice->GetCount() - 1) {
            // Custom option
            cfg.localShell = m_localShellCustomCtrl->GetValue();
        } else {
            wxString choice = m_localShellChoice->GetStringSelection();
            int spacePos = choice.Find(' ');
            if (spacePos != wxNOT_FOUND) {
                cfg.localShell = choice.Left(spacePos);
            } else {
                cfg.localShell = choice;
            }
        }
        
        cfg.termType = m_termTypeCtrl->GetValue();
        
        cfg.telnetHost = m_telnetHostCtrl->GetValue();
        cfg.telnetPort = m_telnetPortSpin->GetValue();
        cfg.telnetRaw = m_telnetRawCheck->GetValue();
        cfg.telnetSSL = m_telnetSSLCheck->GetValue();
        
        cfg.serialPort = m_serialPortCtrl->GetValue();
        int baudSel = m_serialBaudChoice->GetSelection();
        if (baudSel != wxNOT_FOUND) {
            cfg.serialBaud = wxAtoi(m_serialBaudChoice->GetString(baudSel));
        }
        
        cfg.sshUser = m_sshUserCtrl->GetValue();
        cfg.sshHost = m_sshHostCtrl->GetValue();
        cfg.sshPort = m_sshPortSpin->GetValue();
        cfg.sshAuthMethod = m_sshAuthChoice->GetSelection();
        cfg.sshKeyPath = m_sshKeyCtrl->GetValue();
        cfg.sshX11 = m_sshX11Check->GetValue();
        cfg.sshCommand = m_sshCommandCtrl->GetValue();
        cfg.localPF = m_localPFEntries;
        cfg.remotePF = m_remotePFEntries;
        cfg.socks = m_socksEntries;
        
        cfg.webServerEnabled = m_webServerEnabledCheck->GetValue();
        cfg.webServerAddr = m_webServerAddrCtrl->GetValue();
        cfg.webServerPort = m_webServerPortSpin->GetValue();
        cfg.webRootDir = m_webRootDirCtrl->GetValue();
        
        return cfg;
    }

    wxString GetConfigDir() {
        #ifdef __WXMSW__
        return wxEmptyString;  // Use registry on Windows
        #else
        wxString configDir = wxStandardPaths::Get().GetUserConfigDir() + "/FelixTerminal";
        if (!wxDirExists(configDir)) {
            wxMkdir(configDir);
        }
        return configDir;
        #endif
    }

    void LoadSessions() {
        #ifdef __WXMSW__
        wxRegKey regKey(wxRegKey::HKCU, "Software\\FelixTerminal\\Sessions");
        if (!regKey.Exists()) return;
        
        wxString sessionName;
        long index = 0;
        if (regKey.GetFirstKey(sessionName, index)) {
            do {
                m_sessionList->InsertItem(m_sessionList->GetItemCount(), sessionName);
            } while (regKey.GetNextKey(sessionName, index));
        }
        #else
        wxString sessionsDir = GetConfigDir() + "/sessions";
        if (!wxDirExists(sessionsDir)) return;
        
        wxDir dir(sessionsDir);
        wxString filename;
        bool cont = dir.GetFirst(&filename, "*.cfg");
        while (cont) {
            wxString sessionName = filename.BeforeLast('.');
            m_sessionList->InsertItem(m_sessionList->GetItemCount(), sessionName);
            cont = dir.GetNext(&filename);
        }
        #endif
    }

    void OnLoadSession(wxCommandEvent &event) {
        int sel = m_sessionList->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
        if (sel == -1) {
            wxMessageBox("Please select a session", "Error", wxOK | wxICON_ERROR);
            return;
        }

        wxString sessionName = m_sessionList->GetItemText(sel);
        SessionConfig cfg = LoadSessionConfig(sessionName);
        LoadSessionToUI(cfg);
        m_sessionNameCtrl->SetValue(sessionName);
    }

    void OnSaveSession(wxCommandEvent &event) {
        wxString sessionName = m_sessionNameCtrl->GetValue();
        if (sessionName.empty()) {
            wxMessageBox("Please enter a session name", "Error", wxOK | wxICON_ERROR);
            return;
        }

        SessionConfig cfg = GetCurrentConfig(sessionName);
        SaveSessionConfig(cfg);

        bool found = false;
        for (int i = 0; i < (int)m_sessionList->GetItemCount(); i++) {
            if (m_sessionList->GetItemText(i) == sessionName) {
                found = true;
                break;
            }
        }
        if (!found) {
            m_sessionList->InsertItem(m_sessionList->GetItemCount(), sessionName);
        }

        wxMessageBox("Session saved!", "Success", wxOK | wxICON_INFORMATION);
    }

    void OnDeleteSession(wxCommandEvent &event) {
        int sel = m_sessionList->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
        if (sel == -1) {
            wxMessageBox("Please select a session", "Error", wxOK | wxICON_ERROR);
            return;
        }

        wxString sessionName = m_sessionList->GetItemText(sel);
        
        #ifdef __WXMSW__
        wxRegKey regKey(wxRegKey::HKCU, "Software\\FelixTerminal\\Sessions\\" + sessionName);
        if (regKey.Exists()) {
            regKey.DeleteSelf();
            m_sessionList->DeleteItem(sel);
            wxMessageBox("Session deleted!", "Success", wxOK | wxICON_INFORMATION);
        }
        #else
        wxString sessionFile = GetConfigDir() + "/sessions/" + sessionName + ".cfg";
        if (wxFileExists(sessionFile)) {
            wxRemoveFile(sessionFile);
            m_sessionList->DeleteItem(sel);
            wxMessageBox("Session deleted!", "Success", wxOK | wxICON_INFORMATION);
        }
        #endif
    }

    wxString FindFltExecutable() {
    #ifdef __WXMSW__
        const wxString exeName = "flt.exe";
    #else
        const wxString exeName = "flt";
    #endif
        // 1. Same folder as the GUI executable
        wxFileName candidate(wxFileName(wxStandardPaths::Get().GetExecutablePath()).GetPath(), exeName);
        if (candidate.FileExists()) return candidate.GetFullPath();

        // 2. Current working directory
        candidate.Assign(wxGetCwd(), exeName);
        if (candidate.FileExists()) return candidate.GetFullPath();

        // 3. Anywhere on PATH
        wxPathList pathList;
        pathList.AddEnvList("PATH");
        return pathList.FindAbsoluteValidPath(exeName);  // empty if not found
    }

    void OnOpen(wxCommandEvent &event) {
        wxString fltPath = FindFltExecutable();
        if (fltPath.empty()) {
            wxMessageBox(wxString::Format(
                "Could not find '%s'.\n\nPlace it in the same folder as Felix Terminal GUI or on your PATH.",
                wxFileName(FLT_EXECUTABLE).GetFullName()),
                "Felix Terminal Not Found", wxOK | wxICON_ERROR);
            return;
        }

    #ifndef __WXMSW__
        if (!wxFileName::IsFileExecutable(fltPath)) {
            wxMessageBox("'" + fltPath + "' exists but is not executable.\n\nRun: chmod +x " + fltPath,
                         "Permission Error", wxOK | wxICON_ERROR);
            return;
        }
    #endif

        std::vector<wxString> args = BuildArgs();
        wxString cmdLine = QuoteArgForDisplay(fltPath);
        for (const wxString &a : args) cmdLine += " " + QuoteArgForDisplay(a);

    #ifdef __WXMSW__
        // Windows passes one command line to CreateProcess; each argument is
        // quoted by WinQuoteArg so flt's argv gets back the exact values.
        long pid = wxExecute(cmdLine, wxEXEC_ASYNC);
    #else
        // Unix: hand over an argument array, so no quoting/splitting is involved.
        std::vector<wxWCharBuffer> bufs;
        bufs.push_back(fltPath.wc_str());
        for (const wxString &a : args) bufs.push_back(a.wc_str());
        std::vector<wchar_t *> argv;
        for (wxWCharBuffer &b : bufs) argv.push_back(b.data());
        argv.push_back(nullptr);
        long pid = wxExecute(argv.data(), wxEXEC_ASYNC);
    #endif
        if (pid == 0) {
            wxMessageBox("Failed to launch Felix Terminal:\n\n" + cmdLine,
                         "Launch Failed", wxOK | wxICON_ERROR);
            return;
        }

    }

    void OnClear(wxCommandEvent &event) {
        // Reset all fields to defaults
        m_sessionNameCtrl->SetValue("New Session");
        m_connTypeChoice->SetSelection(0);
        
        m_localShellChoice->SetSelection(0);
        m_localShellCustomCtrl->SetValue("");
        m_localShellCustomCtrl->Enable(false);
        
        m_telnetHostCtrl->SetValue("localhost");
        m_telnetPortSpin->SetValue(23);
        m_telnetRawCheck->SetValue(false);
        m_telnetSSLCheck->SetValue(false);
        
        #ifdef __WXMSW__
        m_serialPortCtrl->SetValue("COM1");
        #else
        m_serialPortCtrl->SetValue("/dev/ttyS0");
        #endif
        m_serialBaudChoice->SetSelection(3);
        
        m_sshUserCtrl->SetValue(wxGetenv("USER"));
        m_sshHostCtrl->SetValue("localhost");
        m_sshPortSpin->SetValue(22);
        m_sshAuthChoice->SetSelection(0);
        m_sshKeyCtrl->SetValue("");
        m_sshX11Check->SetValue(true);
        m_sshCommandCtrl->SetValue("");
        
        m_termTypeCtrl->SetValue("xterm-256color");
        
        m_localPFList->Clear();
        m_localPFEntries.clear();
        m_remotePFList->Clear();
        m_remotePFEntries.clear();
        m_socksList->Clear();
        m_socksEntries.clear();
        
        m_webServerEnabledCheck->SetValue(false);
        m_webServerAddrCtrl->SetValue("127.0.0.1");
        m_webServerPortSpin->SetValue(53716);
        m_webRootDirCtrl->SetValue("/");
        
        wxCommandEvent dummy;
        OnSSHAuthChange(dummy);
        UpdatePreview();
    }

    void OnClose(wxCommandEvent &event) {
        Close(true);
    }

    void OnExportSettings(wxCommandEvent &event) {
        SessionConfig cfg = GetCurrentConfig(m_sessionNameCtrl->GetValue());

        wxString defaultName = cfg.name.empty() ? wxString("FelixTerminal") : cfg.name;
        wxFileDialog dlg(this, "Export Settings", "", defaultName + ".cfg",
                          "Felix Terminal Config (*.cfg)|*.cfg|All files (*.*)|*.*",
                          wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
        if (dlg.ShowModal() == wxID_OK) {
            ExportConfigToFile(cfg, dlg.GetPath());
            wxMessageBox("Settings exported!", "Success", wxOK | wxICON_INFORMATION);
        }
    }

    void OnImportSettings(wxCommandEvent &event) {
        wxFileDialog dlg(this, "Import Settings", "", "",
                          "Felix Terminal Config (*.cfg)|*.cfg|All files (*.*)|*.*",
                          wxFD_OPEN | wxFD_FILE_MUST_EXIST);
        if (dlg.ShowModal() == wxID_OK) {
            SessionConfig cfg = ImportConfigFromFile(dlg.GetPath());
            if (cfg.name.empty()) {
                cfg.name = wxFileName(dlg.GetPath()).GetName();
            }
            LoadSessionToUI(cfg);
            m_sessionNameCtrl->SetValue(cfg.name);
            UpdatePreview();
            wxMessageBox("Settings imported!", "Success", wxOK | wxICON_INFORMATION);
        }
    }

    void OnAbout(wxCommandEvent &event) {
        wxDialog dlg(this, wxID_ANY, "About Felix Terminal", wxDefaultPosition, wxSize(520, 620));
        wxBoxSizer *sizer = new wxBoxSizer(wxVERTICAL);

        // Felix's portrait (reuse the app icon) next to the title
        wxBoxSizer *headerSizer = new wxBoxSizer(wxHORIZONTAL);

        wxMemoryInputStream istream(favicon_ico, favicon_ico_len);
        wxImage felixImg(istream, wxBITMAP_TYPE_ICO);
        if (felixImg.IsOk()) {
            felixImg.Rescale(64, 64, wxIMAGE_QUALITY_HIGH);
            wxStaticBitmap *felixPic = new wxStaticBitmap(&dlg, wxID_ANY, wxBitmap(felixImg));
            headerSizer->Add(felixPic, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 12);
        }

        wxStaticText *title = new wxStaticText(&dlg, wxID_ANY, "Felix Terminal");
        wxFont titleFont = title->GetFont();
        titleFont.SetPointSize(titleFont.GetPointSize() + 6);
        titleFont.SetWeight(wxFONTWEIGHT_BOLD);
        title->SetFont(titleFont);
        headerSizer->Add(title, 0, wxALIGN_CENTER_VERTICAL);

        sizer->Add(headerSizer, 0, wxALIGN_CENTER | wxTOP | wxBOTTOM, 12);

        wxString storyText =
            "Felix Terminal is named after Felix, a lovebird whose life was tragically\n"
            "cut short. Felix had a particular fondness for perching on top of\n"
            "computer monitors and supervising whatever was happening on\n"
            "screen -- and occasionally causing as much mischief as a small bird\n"
            "possibly could. In his honor, this terminal carries his name.\n"
            "Every time it opens, a little chaos birdie lives on.\n\n"
            "Rest easy, Felix.";


        wxStaticText *storyCtrl = new wxStaticText(&dlg, wxID_ANY, storyText, wxDefaultPosition,
                                                     wxDefaultSize, wxALIGN_CENTER);
        storyCtrl->Wrap(460);
        sizer->Add(storyCtrl, 0, wxEXPAND | wxLEFT | wxRIGHT, 16);

        // Plain clickable label instead of wxHyperlinkCtrl -- avoids requiring
        // the wxWidgets "adv" library, which some minimal/mingw builds don't link.
        wxStaticText *coffeeLink = new wxStaticText(&dlg, wxID_ANY,
                                                      "Buy me a coffee if you like Felix Terminal");
        wxFont linkFont = coffeeLink->GetFont();
        linkFont.SetUnderlined(true);
        coffeeLink->SetFont(linkFont);
        coffeeLink->SetForegroundColour(wxColour(0, 102, 204));
        coffeeLink->SetCursor(wxCursor(wxCURSOR_HAND));
        coffeeLink->Bind(wxEVT_LEFT_UP, [](wxMouseEvent &) {
            wxLaunchDefaultBrowser("https://buymeacoffee.com/jasonbrianhall");
        });
        sizer->Add(coffeeLink, 0, wxALIGN_CENTER | wxALL, 12);


        wxStaticLine *line = new wxStaticLine(&dlg, wxID_ANY);
        sizer->Add(line, 0, wxEXPAND | wxLEFT | wxRIGHT, 16);

        wxStaticText *licenseTitle = new wxStaticText(&dlg, wxID_ANY, "MIT License");
        wxFont licenseTitleFont = licenseTitle->GetFont();
        licenseTitleFont.SetWeight(wxFONTWEIGHT_BOLD);
        licenseTitle->SetFont(licenseTitleFont);
        sizer->Add(licenseTitle, 0, wxALIGN_CENTER | wxTOP | wxBOTTOM, 8);

        wxString licenseText =
            "Copyright (c) 2026 Jason Brian Hall\n\n"
            "Permission is hereby granted, free of charge, to any person obtaining a copy "
            "of this software and associated documentation files (the \"Software\"), to deal "
            "in the Software without restriction, including without limitation the rights "
            "to use, copy, modify, merge, publish, distribute, sublicense, and/or sell "
            "copies of the Software, and to permit persons to whom the Software is "
            "furnished to do so, subject to the following conditions:\n\n"
            "The above copyright notice and this permission notice shall be included in all "
            "copies or substantial portions of the Software.\n\n"
            "THE SOFTWARE IS PROVIDED \"AS IS\", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR "
            "IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, "
            "FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE "
            "AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER "
            "LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, "
            "OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE "
            "SOFTWARE.";

        wxTextCtrl *licenseCtrl = new wxTextCtrl(&dlg, wxID_ANY, licenseText, wxDefaultPosition,
                                                  wxSize(-1, 160),
                                                  wxTE_MULTILINE | wxTE_READONLY | wxTE_WORDWRAP);
        sizer->Add(licenseCtrl, 1, wxEXPAND | wxALL, 16);

        wxButton *okBtn = new wxButton(&dlg, wxID_OK, "OK");
        sizer->Add(okBtn, 0, wxALIGN_CENTER | wxBOTTOM, 12);

        dlg.SetSizer(sizer);
        dlg.ShowModal();
    }

    wxString GetConfigValue(wxFileConfig &config, const wxString &key, const wxString &defaultVal = "") {
        return config.Read(key, defaultVal);
    }

    long GetConfigLong(wxFileConfig &config, const wxString &key, long defaultVal = 0) {
        return config.ReadLong(key, defaultVal);
    }

    SessionConfig LoadSessionConfig(const wxString &name) {
        SessionConfig cfg;
        cfg.name = name;
        
        #ifdef __WXMSW__
        wxRegKey regKey(wxRegKey::HKCU, "Software\\FelixTerminal\\Sessions\\" + name);
        if (!regKey.Exists()) return cfg;
        
        long lVal;
        wxString sVal;
        
        if (regKey.QueryValue("connType", &lVal)) cfg.connType = lVal;
        if (regKey.QueryValue("localShell", sVal)) cfg.localShell = sVal;
        if (regKey.QueryValue("termType", sVal)) cfg.termType = sVal;
        if (regKey.QueryValue("telnetHost", sVal)) cfg.telnetHost = sVal;
        if (regKey.QueryValue("telnetPort", &lVal)) cfg.telnetPort = lVal;
        if (regKey.QueryValue("telnetTType", sVal)) cfg.telnetTType = sVal;
        if (regKey.QueryValue("telnetRaw", &lVal)) cfg.telnetRaw = (lVal != 0);
        if (regKey.QueryValue("telnetSSL", &lVal)) cfg.telnetSSL = (lVal != 0);
        if (regKey.QueryValue("serialPort", sVal)) cfg.serialPort = sVal;
        if (regKey.QueryValue("serialBaud", &lVal)) cfg.serialBaud = lVal;
        if (regKey.QueryValue("sshUser", sVal)) cfg.sshUser = sVal;
        if (regKey.QueryValue("sshHost", sVal)) cfg.sshHost = sVal;
        if (regKey.QueryValue("sshPort", &lVal)) cfg.sshPort = lVal;
        if (regKey.QueryValue("sshAuthMethod", &lVal)) cfg.sshAuthMethod = lVal;
        if (regKey.QueryValue("sshKeyPath", sVal)) cfg.sshKeyPath = sVal;
        if (regKey.QueryValue("sshKnownHosts", sVal)) cfg.sshKnownHosts = sVal;
        if (regKey.QueryValue("sshX11", &lVal)) cfg.sshX11 = (lVal != 0);
        if (regKey.QueryValue("sshCommand", sVal)) cfg.sshCommand = sVal;
        
        if (regKey.QueryValue("localPF", sVal)) {
            wxArrayString parts = wxSplit(sVal, '|');
            for (const auto &part : parts) {
                if (!part.empty()) cfg.localPF.push_back(part);
            }
        }
        if (regKey.QueryValue("remotePF", sVal)) {
            wxArrayString parts = wxSplit(sVal, '|');
            for (const auto &part : parts) {
                if (!part.empty()) cfg.remotePF.push_back(part);
            }
        }
        if (regKey.QueryValue("socks", sVal)) {
            wxArrayString parts = wxSplit(sVal, '|');
            for (const auto &part : parts) {
                if (!part.empty()) cfg.socks.push_back(part);
            }
        }
        
        if (regKey.QueryValue("webServerEnabled", &lVal)) cfg.webServerEnabled = (lVal != 0);
        if (regKey.QueryValue("webServerAddr", sVal)) cfg.webServerAddr = sVal;
        if (regKey.QueryValue("webServerPort", &lVal)) cfg.webServerPort = lVal;
        if (regKey.QueryValue("webRootDir", sVal)) cfg.webRootDir = sVal;
        
        #else
        wxString sessionFile = GetConfigDir() + "/sessions/" + name + ".cfg";
        if (!wxFileExists(sessionFile)) return cfg;
        
        wxFileConfig config(wxEmptyString, wxEmptyString, sessionFile);
        
        cfg.connType = config.ReadLong("connType", 0);
        cfg.localShell = config.Read("localShell", "");
        cfg.termType = config.Read("termType", "xterm-256color");
        cfg.telnetHost = config.Read("telnetHost", "");
        cfg.telnetPort = config.ReadLong("telnetPort", 23);
        cfg.telnetTType = config.Read("telnetTType", "xterm-256color");
        cfg.telnetRaw = config.ReadLong("telnetRaw", 0) != 0;
        cfg.telnetSSL = config.ReadLong("telnetSSL", 0) != 0;
        cfg.serialPort = config.Read("serialPort", "");
        cfg.serialBaud = config.ReadLong("serialBaud", 9600);
        cfg.sshUser = config.Read("sshUser", "");
        cfg.sshHost = config.Read("sshHost", "localhost");
        cfg.sshPort = config.ReadLong("sshPort", 22);
        cfg.sshAuthMethod = config.ReadLong("sshAuthMethod", 0);
        cfg.sshKeyPath = config.Read("sshKeyPath", "");
        cfg.sshKnownHosts = config.Read("sshKnownHosts", "");
        cfg.sshX11 = config.ReadLong("sshX11", 1) != 0;
        cfg.sshCommand = config.Read("sshCommand", "");
        
        wxString localPFStr = config.Read("localPF", "");
        if (!localPFStr.empty()) {
            wxArrayString parts = wxSplit(localPFStr, '|');
            for (const auto &part : parts) {
                if (!part.empty()) cfg.localPF.push_back(part);
            }
        }
        
        wxString remotePFStr = config.Read("remotePF", "");
        if (!remotePFStr.empty()) {
            wxArrayString parts = wxSplit(remotePFStr, '|');
            for (const auto &part : parts) {
                if (!part.empty()) cfg.remotePF.push_back(part);
            }
        }
        
        wxString socksStr = config.Read("socks", "");
        if (!socksStr.empty()) {
            wxArrayString parts = wxSplit(socksStr, '|');
            for (const auto &part : parts) {
                if (!part.empty()) cfg.socks.push_back(part);
            }
        }
        
        cfg.webServerEnabled = config.ReadLong("webServerEnabled", 0) != 0;
        cfg.webServerAddr = config.Read("webServerAddr", "127.0.0.1");
        cfg.webServerPort = config.ReadLong("webServerPort", 53716);
        cfg.webRootDir = config.Read("webRootDir", "/");
        #endif
        
        return cfg;
    }

    void SaveSessionConfig(const SessionConfig &cfg) {
        // Store port forwards and SOCKS as concatenated strings
        wxString localPFStr, remotePFStr, socksStr;
        for (size_t i = 0; i < cfg.localPF.size(); i++) {
            if (i > 0) localPFStr += "|";
            localPFStr += cfg.localPF[i];
        }
        for (size_t i = 0; i < cfg.remotePF.size(); i++) {
            if (i > 0) remotePFStr += "|";
            remotePFStr += cfg.remotePF[i];
        }
        for (size_t i = 0; i < cfg.socks.size(); i++) {
            if (i > 0) socksStr += "|";
            socksStr += cfg.socks[i];
        }
        
        #ifdef __WXMSW__
        wxRegKey regKey(wxRegKey::HKCU, "Software\\FelixTerminal\\Sessions\\" + cfg.name);
        if (!regKey.Exists()) regKey.Create();
        
        regKey.SetValue("connType", (long)cfg.connType);
        regKey.SetValue("localShell", cfg.localShell);
        regKey.SetValue("termType", cfg.termType);
        regKey.SetValue("telnetHost", cfg.telnetHost);
        regKey.SetValue("telnetPort", (long)cfg.telnetPort);
        regKey.SetValue("telnetTType", cfg.telnetTType);
        regKey.SetValue("telnetRaw", cfg.telnetRaw ? 1L : 0L);
        regKey.SetValue("telnetSSL", cfg.telnetSSL ? 1L : 0L);
        regKey.SetValue("serialPort", cfg.serialPort);
        regKey.SetValue("serialBaud", (long)cfg.serialBaud);
        regKey.SetValue("sshUser", cfg.sshUser);
        regKey.SetValue("sshHost", cfg.sshHost);
        regKey.SetValue("sshPort", (long)cfg.sshPort);
        regKey.SetValue("sshAuthMethod", (long)cfg.sshAuthMethod);
        regKey.SetValue("sshKeyPath", cfg.sshKeyPath);
        regKey.SetValue("sshKnownHosts", cfg.sshKnownHosts);
        regKey.SetValue("sshX11", cfg.sshX11 ? 1L : 0L);
        regKey.SetValue("sshCommand", cfg.sshCommand);
        regKey.SetValue("localPF", localPFStr);
        regKey.SetValue("remotePF", remotePFStr);
        regKey.SetValue("socks", socksStr);
        regKey.SetValue("webServerEnabled", cfg.webServerEnabled ? 1L : 0L);
        regKey.SetValue("webServerAddr", cfg.webServerAddr);
        regKey.SetValue("webServerPort", (long)cfg.webServerPort);
        regKey.SetValue("webRootDir", cfg.webRootDir);
        
        #else
        wxString sessionsDir = GetConfigDir() + "/sessions";
        if (!wxDirExists(sessionsDir)) {
            wxMkdir(sessionsDir);
        }
        
        wxString sessionFile = sessionsDir + "/" + cfg.name + ".cfg";
        wxFileConfig config(wxEmptyString, wxEmptyString, sessionFile);
        
        config.Write("connType", (long)cfg.connType);
        config.Write("localShell", cfg.localShell);
        config.Write("termType", cfg.termType);
        config.Write("telnetHost", cfg.telnetHost);
        config.Write("telnetPort", (long)cfg.telnetPort);
        config.Write("telnetTType", cfg.telnetTType);
        config.Write("telnetRaw", cfg.telnetRaw ? 1L : 0L);
        config.Write("telnetSSL", cfg.telnetSSL ? 1L : 0L);
        config.Write("serialPort", cfg.serialPort);
        config.Write("serialBaud", (long)cfg.serialBaud);
        config.Write("sshUser", cfg.sshUser);
        config.Write("sshHost", cfg.sshHost);
        config.Write("sshPort", (long)cfg.sshPort);
        config.Write("sshAuthMethod", (long)cfg.sshAuthMethod);
        config.Write("sshKeyPath", cfg.sshKeyPath);
        config.Write("sshKnownHosts", cfg.sshKnownHosts);
        config.Write("sshX11", cfg.sshX11 ? 1L : 0L);
        config.Write("sshCommand", cfg.sshCommand);
        config.Write("localPF", localPFStr);
        config.Write("remotePF", remotePFStr);
        config.Write("socks", socksStr);
        config.Write("webServerEnabled", cfg.webServerEnabled ? 1L : 0L);
        config.Write("webServerAddr", cfg.webServerAddr);
        config.Write("webServerPort", (long)cfg.webServerPort);
        config.Write("webRootDir", cfg.webRootDir);
        config.Flush();
        #endif
    }

    // Export/Import are always file-based (portable), regardless of platform,
    // so settings can be shared between machines / backed up.
    void ExportConfigToFile(const SessionConfig &cfg, const wxString &path) {
        wxString localPFStr, remotePFStr, socksStr;
        for (size_t i = 0; i < cfg.localPF.size(); i++) {
            if (i > 0) localPFStr += "|";
            localPFStr += cfg.localPF[i];
        }
        for (size_t i = 0; i < cfg.remotePF.size(); i++) {
            if (i > 0) remotePFStr += "|";
            remotePFStr += cfg.remotePF[i];
        }
        for (size_t i = 0; i < cfg.socks.size(); i++) {
            if (i > 0) socksStr += "|";
            socksStr += cfg.socks[i];
        }

        if (wxFileExists(path)) {
            wxRemoveFile(path);
        }
        wxFileConfig config(wxEmptyString, wxEmptyString, path);

        config.Write("name", cfg.name);
        config.Write("connType", (long)cfg.connType);
        config.Write("localShell", cfg.localShell);
        config.Write("termType", cfg.termType);
        config.Write("telnetHost", cfg.telnetHost);
        config.Write("telnetPort", (long)cfg.telnetPort);
        config.Write("telnetTType", cfg.telnetTType);
        config.Write("telnetRaw", cfg.telnetRaw ? 1L : 0L);
        config.Write("telnetSSL", cfg.telnetSSL ? 1L : 0L);
        config.Write("serialPort", cfg.serialPort);
        config.Write("serialBaud", (long)cfg.serialBaud);
        config.Write("sshUser", cfg.sshUser);
        config.Write("sshHost", cfg.sshHost);
        config.Write("sshPort", (long)cfg.sshPort);
        config.Write("sshAuthMethod", (long)cfg.sshAuthMethod);
        config.Write("sshKeyPath", cfg.sshKeyPath);
        config.Write("sshKnownHosts", cfg.sshKnownHosts);
        config.Write("sshX11", cfg.sshX11 ? 1L : 0L);
        config.Write("sshCommand", cfg.sshCommand);
        config.Write("localPF", localPFStr);
        config.Write("remotePF", remotePFStr);
        config.Write("socks", socksStr);
        config.Write("webServerEnabled", cfg.webServerEnabled ? 1L : 0L);
        config.Write("webServerAddr", cfg.webServerAddr);
        config.Write("webServerPort", (long)cfg.webServerPort);
        config.Write("webRootDir", cfg.webRootDir);
        config.Flush();
    }

    SessionConfig ImportConfigFromFile(const wxString &path) {
        SessionConfig cfg;
        if (!wxFileExists(path)) return cfg;

        wxFileConfig config(wxEmptyString, wxEmptyString, path);

        cfg.name = config.Read("name", "");
        cfg.connType = config.ReadLong("connType", 0);
        cfg.localShell = config.Read("localShell", "");
        cfg.termType = config.Read("termType", "xterm-256color");
        cfg.telnetHost = config.Read("telnetHost", "");
        cfg.telnetPort = config.ReadLong("telnetPort", 23);
        cfg.telnetTType = config.Read("telnetTType", "xterm-256color");
        cfg.telnetRaw = config.ReadLong("telnetRaw", 0) != 0;
        cfg.telnetSSL = config.ReadLong("telnetSSL", 0) != 0;
        cfg.serialPort = config.Read("serialPort", "");
        cfg.serialBaud = config.ReadLong("serialBaud", 9600);
        cfg.sshUser = config.Read("sshUser", "");
        cfg.sshHost = config.Read("sshHost", "localhost");
        cfg.sshPort = config.ReadLong("sshPort", 22);
        cfg.sshAuthMethod = config.ReadLong("sshAuthMethod", 0);
        cfg.sshKeyPath = config.Read("sshKeyPath", "");
        cfg.sshKnownHosts = config.Read("sshKnownHosts", "");
        cfg.sshX11 = config.ReadLong("sshX11", 1) != 0;
        cfg.sshCommand = config.Read("sshCommand", "");

        wxString localPFStr = config.Read("localPF", "");
        if (!localPFStr.empty()) {
            wxArrayString parts = wxSplit(localPFStr, '|');
            for (const auto &part : parts) {
                if (!part.empty()) cfg.localPF.push_back(part);
            }
        }

        wxString remotePFStr = config.Read("remotePF", "");
        if (!remotePFStr.empty()) {
            wxArrayString parts = wxSplit(remotePFStr, '|');
            for (const auto &part : parts) {
                if (!part.empty()) cfg.remotePF.push_back(part);
            }
        }

        wxString socksStr = config.Read("socks", "");
        if (!socksStr.empty()) {
            wxArrayString parts = wxSplit(socksStr, '|');
            for (const auto &part : parts) {
                if (!part.empty()) cfg.socks.push_back(part);
            }
        }

        cfg.webServerEnabled = config.ReadLong("webServerEnabled", 0) != 0;
        cfg.webServerAddr = config.Read("webServerAddr", "127.0.0.1");
        cfg.webServerPort = config.ReadLong("webServerPort", 53716);
        cfg.webRootDir = config.Read("webRootDir", "/");

        return cfg;
    }

    void LoadSessionToUI(const SessionConfig &cfg) {
        m_connTypeChoice->SetSelection(cfg.connType);
        
        // Set local shell choice
        bool found = false;
        for (int i = 0; i < (int)m_localShellChoice->GetCount() - 1; i++) {
            wxString choice = m_localShellChoice->GetString(i);
            int spacePos = choice.Find(' ');
            wxString shellPath = (spacePos != wxNOT_FOUND) ? choice.Left(spacePos) : choice;
            if (shellPath == cfg.localShell) {
                m_localShellChoice->SetSelection(i);
                found = true;
                break;
            }
        }
        if (!found) {
            // Custom shell
            m_localShellChoice->SetSelection(m_localShellChoice->GetCount() - 1);
            m_localShellCustomCtrl->SetValue(cfg.localShell);
        }
        m_localShellCustomCtrl->Enable(false);
        
        m_telnetHostCtrl->SetValue(cfg.telnetHost);
        m_telnetPortSpin->SetValue(cfg.telnetPort);
        m_telnetRawCheck->SetValue(cfg.telnetRaw);
        m_telnetSSLCheck->SetValue(cfg.telnetSSL);
        
        m_serialPortCtrl->SetValue(cfg.serialPort);
        // Find and select the matching baud rate
        for (int i = 0; i < (int)m_serialBaudChoice->GetCount(); i++) {
            if (wxAtoi(m_serialBaudChoice->GetString(i)) == cfg.serialBaud) {
                m_serialBaudChoice->SetSelection(i);
                break;
            }
        }
        
        m_sshUserCtrl->SetValue(cfg.sshUser);
        m_sshHostCtrl->SetValue(cfg.sshHost);
        m_sshPortSpin->SetValue(cfg.sshPort);
        m_sshAuthChoice->SetSelection(cfg.sshAuthMethod);
        m_sshKeyCtrl->SetValue(cfg.sshKeyPath);
        m_sshX11Check->SetValue(cfg.sshX11);
        m_sshCommandCtrl->SetValue(cfg.sshCommand);
        
        m_termTypeCtrl->SetValue(cfg.termType);
        
        m_localPFList->Clear();
        m_localPFEntries.clear();
        for (const auto &pf : cfg.localPF) {
            m_localPFList->Append(pf);
            m_localPFEntries.push_back(pf);
        }
        
        m_remotePFList->Clear();
        m_remotePFEntries.clear();
        for (const auto &pf : cfg.remotePF) {
            m_remotePFList->Append(pf);
            m_remotePFEntries.push_back(pf);
        }
        
        m_socksList->Clear();
        m_socksEntries.clear();
        for (const auto &s : cfg.socks) {
            m_socksList->Append(s);
            m_socksEntries.push_back(s);
        }
        
        m_webServerEnabledCheck->SetValue(cfg.webServerEnabled);
        m_webServerAddrCtrl->SetValue(cfg.webServerAddr);
        m_webServerPortSpin->SetValue(cfg.webServerPort);
        m_webRootDirCtrl->SetValue(cfg.webRootDir);
        
        wxCommandEvent dummy;
        OnSSHAuthChange(dummy);
        OnConnTypeChange(dummy);
        UpdatePreview();
    }
};

class FelixApp : public wxApp {
public:
    bool OnInit() override {
        wxImage::AddHandler(new wxICOHandler());
        FelixTerminalFrame *frame = new FelixTerminalFrame();
        frame->Show();
        return true;
    }
};

wxIMPLEMENT_APP(FelixApp);
