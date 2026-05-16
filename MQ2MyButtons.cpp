// MQ2MyButtons.cpp
// Original by unknown, updated by Knightly, condensed/extended by Nerp, vastly assisted by Claude
//  

#include <mq/Plugin.h>
#include <mq/imgui/ImGuiUtils.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

PreSetup("MQ2MyButtons");
PLUGIN_VERSION(2026.515);

PLUGIN_API void MyButtonsCommand(SPAWNINFO* pSpawn, char* szLine);

constexpr int MAX_BUTTONS  = 60;
constexpr int XML_SCHEMA   = 3; // bump when XML generation logic changes to invalidate cached files
static const char* XML_FILE = "MQUI_MyButtonsWnd.xml";

struct ButtonDef {
    char label[MAX_STRING]   = {};
    char command[MAX_STRING] = {};
    int  r = 255, g = 255, b = 255;
};

static int g_numButtons   = 30;
static int g_buttonWidth  = 40;
static int g_buttonHeight = 18;

// settings the current XML on disk was built with (0 = unknown)
static int g_xmlNum    = 0;
static int g_xmlWidth  = 0;
static int g_xmlHeight = 0;

// button editor state (0 = not editing)
static int   g_editingButton            = 0;
static char  g_editLabelBuf[MAX_STRING] = {};
static char  g_editCmdBuf[MAX_STRING]   = {};
static float g_editColor[3]             = { 1.0f, 1.0f, 1.0f };

// broadcast settings
static bool g_broadcastEnabled = false;
static int  g_broadcastMethod  = 0; // 0 = DanNet (/dgze), 1 = EQBC (/bca)

static std::array<ButtonDef, MAX_BUTTONS + 1> g_buttons; // 1-indexed

class CHButWnd;
static CHButWnd*        MyBtnWnd       = nullptr;
class MQ2MyButtonsType* pMyButtonsType = nullptr;

// -------------------------------------------------------------------------
// Logging
// -------------------------------------------------------------------------
static void MBLog(const std::string& msg)
{
    WriteChatf("\ay[\agMQ2MyButtons\ay]\aw ::: \ao%s", msg.c_str());
}

static void MBError(const std::string& msg)
{
    WriteChatf("\ay[\agMQ2MyButtons\ay]\aw ::: \arERROR: %s", msg.c_str());
}

// -------------------------------------------------------------------------
// Pre-parse local tokens
//
// ^{expression} is expanded on this client before the command is dispatched.
// ${expression} is left intact so /noparse /bcaa can deliver it to receivers.
//
// Example: /noparse /bcaa //if ( ${Raid.MainAssist[1]} == ^{Me.Name} ) /nav id ^{Me.ID}
// Becomes: /noparse /bcaa //if ( ${Raid.MainAssist[1]} == Tark ) /nav id 12345
// Each receiver then parses ${Raid.MainAssist[1]} for their own character.
// -------------------------------------------------------------------------
static void PreParseLocalTokens(char* buf, size_t bufSize)
{
    std::string in(buf), out;
    out.reserve(in.size());
    size_t i = 0;
    while (i < in.size()) {
        if (i + 1 < in.size() && in[i] == '^' && in[i + 1] == '{') {
            int depth = 1;
            size_t j = i + 2;
            while (j < in.size() && depth > 0) {
                if      (in[j] == '{') ++depth;
                else if (in[j] == '}') --depth;
                ++j;
            }
            if (depth == 0) {
                // Wrap inner expression as ${...} and expand it
                std::string expr = "${" + in.substr(i + 2, j - i - 3) + "}";
                char tmp[MAX_STRING];
                strcpy_s(tmp, expr.c_str());
                ParseMacroData(tmp, MAX_STRING);
                out += tmp;
                i = j;
            } else {
                out += in[i++]; // unmatched ^{ — pass through literally
            }
        } else {
            out += in[i++];
        }
    }
    strcpy_s(buf, bufSize, out.c_str());
}

// Convert storage format (literal \n) ↔ display format (actual newlines for ImGui multiline)
static void StorageToDisplay(const char* src, char* dst, size_t dstSize)
{
    std::string out;
    for (size_t i = 0; i < strlen(src); ) {
        if (src[i] == '\\' && src[i + 1] == 'n') { out += '\n'; i += 2; }
        else                                        { out += src[i++]; }
    }
    strcpy_s(dst, dstSize, out.c_str());
}

static void DisplayToStorage(const char* src, char* dst, size_t dstSize)
{
    std::string out;
    for (const char* p = src; *p; ++p)
        out += (*p == '\n') ? "\\n" : std::string(1, *p);
    strcpy_s(dst, dstSize, out.c_str());
}

// Execute a (possibly multi-line) button command.
// Lines are separated by the literal two-character sequence \n in storage.
// Lines starting with # are treated as comments and skipped.
static void ExecuteButtonCmd(SPAWNINFO* pSpawn, const char* rawCmd)
{
    char buf[MAX_STRING];
    strcpy_s(buf, rawCmd);
    PreParseLocalTokens(buf, MAX_STRING);

    const auto execLine = [&](std::string line) {
        const size_t first = line.find_first_not_of(" \t\r");
        if (first == std::string::npos) return;
        line = line.substr(first);
        if (!line.empty() && line[0] != '#')
            DoCommand(pSpawn, line.c_str());
    };

    std::string s(buf);
    size_t start = 0, pos;
    while ((pos = s.find("\\n", start)) != std::string::npos) {
        execLine(s.substr(start, pos - start));
        start = pos + 2;
    }
    execLine(s.substr(start));
}

// -------------------------------------------------------------------------
// INI helpers
// -------------------------------------------------------------------------
static void LoadButtonData()
{
    g_numButtons       = std::clamp(GetPrivateProfileInt("Settings", "MaxButtons",      30,    INIFileName), 10, MAX_BUTTONS);
    g_buttonWidth      = std::clamp(GetPrivateProfileInt("Settings", "ButtonWidth",  40, INIFileName), 1, 400);
    g_buttonHeight     = std::clamp(GetPrivateProfileInt("Settings", "ButtonHeight", 18, INIFileName), 1, 400);
    g_broadcastEnabled = GetPrivateProfileBool("Settings", "BroadcastEnabled", true, INIFileName);
    g_broadcastMethod  = GetPrivateProfileInt( "Settings", "BroadcastMethod",  0,     INIFileName);

    for (int i = 1; i <= MAX_BUTTONS; i++) {
        const std::string section = "Button" + std::to_string(i);
        GetPrivateProfileString(section.c_str(), "Label",   "", g_buttons[i].label,   MAX_STRING, INIFileName);
        GetPrivateProfileString(section.c_str(), "Command", "", g_buttons[i].command, MAX_STRING, INIFileName);
        g_buttons[i].r = GetPrivateProfileInt(section.c_str(), "Red",   255, INIFileName);
        g_buttons[i].g = GetPrivateProfileInt(section.c_str(), "Green", 255, INIFileName);
        g_buttons[i].b = GetPrivateProfileInt(section.c_str(), "Blue",  255, INIFileName);
    }

    if (g_buttons[1].command[0] == '\0') {
        strcpy_s(g_buttons[1].label,   "Help");
        strcpy_s(g_buttons[1].command, "/mybuttons help");
        g_buttons[1].r = 255; g_buttons[1].g = 255; g_buttons[1].b = 153;
        WritePrivateProfileString("Button1", "Label",   g_buttons[1].label,   INIFileName);
        WritePrivateProfileString("Button1", "Command", g_buttons[1].command, INIFileName);
        WritePrivateProfileInt(   "Button1", "Red",     g_buttons[1].r,       INIFileName);
        WritePrivateProfileInt(   "Button1", "Green",   g_buttons[1].g,       INIFileName);
        WritePrivateProfileInt(   "Button1", "Blue",    g_buttons[1].b,       INIFileName);
    }

    if (g_buttons[2].command[0] == '\0') {
        strcpy_s(g_buttons[2].label,   "MQP On");
        strcpy_s(g_buttons[2].command, "/multiline ; /noparse /bcaa //docommand /${Me.Class.ShortName} pause 1 ; /bcaa //mqp on");
        g_buttons[2].r = 255; g_buttons[2].g = 0; g_buttons[2].b = 0;
        WritePrivateProfileString("Button2", "Label",   g_buttons[2].label,   INIFileName);
        WritePrivateProfileString("Button2", "Command", g_buttons[2].command, INIFileName);
        WritePrivateProfileInt(   "Button2", "Red",     g_buttons[2].r,       INIFileName);
        WritePrivateProfileInt(   "Button2", "Green",   g_buttons[2].g,       INIFileName);
        WritePrivateProfileInt(   "Button2", "Blue",    g_buttons[2].b,       INIFileName);
    }

    if (g_buttons[3].command[0] == '\0') {
        strcpy_s(g_buttons[3].label,   "MQP Off");
        strcpy_s(g_buttons[3].command, "/multiline ; /noparse /bcaa //docommand /${Me.Class.ShortName} pause 0 ; /bcaa //mqp off");
        g_buttons[3].r = 0; g_buttons[3].g = 255; g_buttons[3].b = 0;
        WritePrivateProfileString("Button3", "Label",   g_buttons[3].label,   INIFileName);
        WritePrivateProfileString("Button3", "Command", g_buttons[3].command, INIFileName);
        WritePrivateProfileInt(   "Button3", "Red",     g_buttons[3].r,       INIFileName);
        WritePrivateProfileInt(   "Button3", "Green",   g_buttons[3].g,       INIFileName);
        WritePrivateProfileInt(   "Button3", "Blue",    g_buttons[3].b,       INIFileName);
    }
}

// -------------------------------------------------------------------------
// XML generation
//
// All buttons share the same sprite area (row 0 of window_pieces06.tga).
// X offset selects the state: 0=Normal, 40=Flyby, 80=Pressed, 120=PressedFlyby, 160=Disabled.
// 60 buttons use window_pieces06.tga through window_pieces15.tga.
// -------------------------------------------------------------------------
static std::string AnimXML(int btn, const char* state, int xOff)
{
    // All buttons reuse row 0 of window_pieces06.tga so we never stray into other UI graphics
    const char* tex  = "window_pieces06.tga";
    const int   yOff = 0;

    std::ostringstream o;
    o << "<Ui2DAnimation item=\"AMQMB_Button" << btn << state << "\">"
      << "<Cycle>true</Cycle><Frames>"
      << "<Texture>" << tex << "</Texture>"
      << "<Location><X>" << xOff << "</X><Y>" << yOff << "</Y></Location>"
      << "<Size><CX>40</CX><CY>40</CY></Size>"
      << "<Hotspot><X>0</X><Y>0</Y></Hotspot>"
      << "<Duration>1000</Duration>"
      << "</Frames></Ui2DAnimation>\n";
    return o.str();
}

static bool GenerateXMLFile()
{
    namespace fs = std::filesystem;
    const fs::path xmlPath = fs::path(gPathResources) / "uifiles\\default\\" / XML_FILE;
    std::error_code ec;
    if (!fs::exists(xmlPath.parent_path(), ec))
        fs::create_directories(xmlPath.parent_path(), ec);

    std::ofstream f(xmlPath);
    if (!f) return false;

    const int w  = g_buttonWidth;
    const int h  = g_buttonHeight;
    const int dW = w - 4;
    const int dH = h - 4;

    f << "<!-- MQ2MyButtons s=" << XML_SCHEMA << " n=" << g_numButtons << " w=" << w << " h=" << h << " -->\n"
      << "<?xml version=\"1.0\" encoding=\"us-ascii\"?>\n"
      << "<XML ID=\"EQInterfaceDefinitionLanguage\">\n"
      << "<Schema xmlns=\"EverQuestData\" xmlns:dt=\"EverQuestDataTypes\" />\n";

    for (int i = 1; i <= g_numButtons; i++) {
        f << AnimXML(i, "Normal",        0);
        f << AnimXML(i, "Pressed",      80);
        f << AnimXML(i, "Flyby",        40);
        f << AnimXML(i, "PressedFlyby",120);
        f << AnimXML(i, "Disabled",    160);
    }

    for (int i = 1; i <= g_numButtons; i++) {
        const std::string n = std::to_string(i);

        f << "<Button item=\"MQMB_Button" << n << "\">"
          << "<ScreenID>MQMB_Button" << n << "</ScreenID>"
          << "<Font>1</Font><RelativePosition>true</RelativePosition>"
          << "<Size><CX>" << w << "</CX><CY>" << h << "</CY></Size><Text></Text>"
          << "<DecalOffset><X>2</X><Y>2</Y></DecalOffset>"
          << "<DecalSize><CX>" << dW << "</CX><CY>" << dH << "</CY></DecalSize>"
          << "<ButtonDrawTemplate>"
          << "<Normal>AMQMB_Button"      << n << "Normal</Normal>"
          << "<Pressed>AMQMB_Button"     << n << "Pressed</Pressed>"
          << "<Flyby>AMQMB_Button"       << n << "Flyby</Flyby>"
          << "<Disabled>AMQMB_Button"    << n << "Disabled</Disabled>"
          << "<PressedFlyby>AMQMB_Button" << n << "PressedFlyby</PressedFlyby>"
          << "</ButtonDrawTemplate></Button>\n";

        f << "<Label item=\"MQMB_Label" << n << "\">"
          << "<ScreenID>MQMB_Label" << n << "</ScreenID>"
          << "<TooltipReference>${MyButtons.Label[" << n << "]}</TooltipReference>"
          << "<RelativePosition>true</RelativePosition>"
          << "<Location><X>0</X><Y>5</Y></Location>"
          << "<Size><CX>" << w << "</CX><CY>" << h << "</CY></Size>"
          << "<Text></Text><Font>1</Font>"
          << "<TextColor>"
          << "<R>" << g_buttons[i].r << "</R>"
          << "<G>" << g_buttons[i].g << "</G>"
          << "<B>" << g_buttons[i].b << "</B>"
          << "</TextColor>"
          << "<NoWrap>false</NoWrap><AlignCenter>true</AlignCenter><AlignRight>false</AlignRight>"
          << "<Style_Transparent>true</Style_Transparent>"
          << "<Style_TransparentControl>true</Style_TransparentControl>"
          << "</Label>\n";

        f << "<LayoutBox item=\"MQMB_LayoutB" << n << "\">"
          << "<ScreenID>MQMB_LayoutB" << n << "</ScreenID>"
          << "<RelativePosition>true</RelativePosition>"
          << "<Size><CX>" << w << "</CX><CY>" << h << "</CY></Size>"
          << "<Style_Transparent>true</Style_Transparent>"
          << "<Style_TransparentControl>true</Style_TransparentControl>"
          << "<Pieces>MQMB_Button" << n << "</Pieces>"
          << "<Pieces>MQMB_Label"  << n << "</Pieces>"
          << "</LayoutBox>\n";
    }

    f << "<TileLayoutBox item=\"MQMB_ButtonLayout\">"
      << "<ScreenID>MQMB_ButtonLayout</ScreenID>"
      << "<RelativePosition>true</RelativePosition><AutoStretch>true</AutoStretch>"
      << "<TopAnchorOffset>4</TopAnchorOffset><BottomAnchorOffset>4</BottomAnchorOffset>"
      << "<LeftAnchorOffset>4</LeftAnchorOffset><RightAnchorOffset>11</RightAnchorOffset>"
      << "<TopAnchorToTop>true</TopAnchorToTop><BottomAnchorToTop>false</BottomAnchorToTop>"
      << "<LeftAnchorToLeft>true</LeftAnchorToLeft><RightAnchorToLeft>false</RightAnchorToLeft>"
      << "<Style_Transparent>true</Style_Transparent>"
      << "<Style_TransparentControl>true</Style_TransparentControl>"
      << "<Spacing>4</Spacing><SecondarySpacing>4</SecondarySpacing>"
      << "<HorizontalFirst>true</HorizontalFirst>"
      << "<AnchorToTop>true</AnchorToTop><AnchorToLeft>true</AnchorToLeft>"
      << "<FirstPieceTemplate>true</FirstPieceTemplate><SnapToChildren>true</SnapToChildren>\n";
    for (int i = 1; i <= g_numButtons; i++)
        f << "<Pieces>LayoutBox:MQMB_LayoutB" << i << "</Pieces>\n";
    f << "</TileLayoutBox>\n";

    f << "<LayoutVertical item=\"MQMB_LayoutV\">"
      << "<ResizeVertical>true</ResizeVertical><ResizeHorizontal>true</ResizeHorizontal>"
      << "</LayoutVertical>\n";

    f << "<Screen item=\"MQMBButtonWnd\"><ScreenID />"
      << "<Layout>MQMB_LayoutV</Layout>"
      << "<Font>2</Font><RelativePosition>false</RelativePosition>"
      << "<Location><X>0</X><Y>230</Y></Location>"
      << "<Size><CX>" << (10 * (w + 4) - 4 + 41) << "</CX><CY>53</CY></Size>"
      << "<DrawTemplate>WDT_RoundedNoTitle</DrawTemplate>"
      << "<Style_Qmarkbox>true</Style_Qmarkbox>"
      << "<Style_Closebox>true</Style_Closebox>"
      << "<Style_Border>true</Style_Border>"
      << "<Style_Sizable>true</Style_Sizable>"
      << "<Style_ClientMovable>true</Style_ClientMovable>"
      << "<Escapable>false</Escapable>"
      << "<Pieces>TileLayoutBox:MQMB_ButtonLayout</Pieces>"
      << "</Screen>\n"
      << "</XML>\n";

    const bool ok = f.good();
    if (ok) { g_xmlNum = g_numButtons; g_xmlWidth = g_buttonWidth; g_xmlHeight = g_buttonHeight; }
    return ok;
}

// Regenerate only when version comment doesn't match current settings
static bool EnsureXMLFile()
{
    namespace fs = std::filesystem;
    const fs::path xmlPath = fs::path(gPathResources) / "uifiles\\default\\" / XML_FILE;
    std::error_code ec;

    if (fs::exists(xmlPath, ec)) {
        std::ifstream f(xmlPath);
        std::string firstLine;
        std::getline(f, firstLine);
        const std::string expected = "<!-- MQ2MyButtons s=" + std::to_string(XML_SCHEMA)
                                   + " n=" + std::to_string(g_numButtons)
                                   + " w=" + std::to_string(g_buttonWidth)
                                   + " h=" + std::to_string(g_buttonHeight) + " -->";
        if (firstLine == expected) {
            g_xmlNum = g_numButtons; g_xmlWidth = g_buttonWidth; g_xmlHeight = g_buttonHeight;
            return true;
        }
    }

    return GenerateXMLFile();
}

// -------------------------------------------------------------------------
// Window class
// -------------------------------------------------------------------------
class CHButWnd : public CCustomWnd
{
public:
    std::array<CButtonWnd*, MAX_BUTTONS + 1> MyButton = {};

    CHButWnd() : CCustomWnd("MQMBButtonWnd")
    {
        for (int i = 1; i <= g_numButtons; i++)
            MyButton[i] = (CButtonWnd*)GetChildItem(("MQMB_Button" + std::to_string(i)).c_str());
    }

    int WndNotification(CXWnd* pWnd, uint32_t uiMessage, void* pData) override
    {
        if (uiMessage == XWM_LCLICK) {
            for (int i = 1; i <= g_numButtons; i++) {
                if (pWnd == MyButton[i]) {
                    if (g_buttons[i].command[0])
                        ExecuteButtonCmd(pCharSpawn, g_buttons[i].command);
                    return 0;
                }
            }
        }
        if (uiMessage == XWM_RCLICK) {
            for (int i = 1; i <= g_numButtons; i++) {
                if (pWnd == MyButton[i]) {
                    g_editingButton = i;
                    strcpy_s(g_editLabelBuf, g_buttons[i].label);
                    StorageToDisplay(g_buttons[i].command, g_editCmdBuf, sizeof(g_editCmdBuf));
                    g_editColor[0] = g_buttons[i].r / 255.0f;
                    g_editColor[1] = g_buttons[i].g / 255.0f;
                    g_editColor[2] = g_buttons[i].b / 255.0f;
                    return 0;
                }
            }
        }
        return 0;
    }

    void SetLabel(int i)
    {
        if (CXWnd* lbl = GetChildItem(("MQMB_Label" + std::to_string(i)).c_str())) {
            char buf[MAX_STRING];
            strcpy_s(buf, g_buttons[i].label);
            ParseMacroData(buf, MAX_STRING);
            lbl->SetWindowText(buf);
        }
    }

    void SetButtonInfo()
    {
        for (int i = 1; i <= g_numButtons; i++) {
            SetLabel(i);
            if (CXWnd* lbl = GetChildItem(("MQMB_Label" + std::to_string(i)).c_str())) {
                ARGBCOLOR c{};
                c.A = GetAlpha();
                c.R = g_buttons[i].r;
                c.G = g_buttons[i].g;
                c.B = g_buttons[i].b;
                lbl->SetCRNormal(c.ARGB);
            }
            if (MyButton[i])
                MyButton[i]->SetTooltip(g_buttons[i].command);
        }
    }
};

// -------------------------------------------------------------------------
// Window persistence
// -------------------------------------------------------------------------
static void ReadWindowINI(CSidlScreenWnd* w)
{
    w->SetLocation({ GetPrivateProfileInt("Location", "Left",   487, INIFileName),
                     GetPrivateProfileInt("Location", "Top",     85, INIFileName),
                     GetPrivateProfileInt("Location", "Right",  964, INIFileName),
                     GetPrivateProfileInt("Location", "Bottom", 138, INIFileName) });
    w->SetLocked(GetPrivateProfileBool("UISettings",    "Locked",      false, INIFileName));
    w->SetFades(GetPrivateProfileBool("UISettings",     "Fades",       false, INIFileName));
    w->SetFadeDelay(GetPrivateProfileInt("UISettings",  "Delay",       2000,  INIFileName));
    w->SetFadeDuration(GetPrivateProfileInt("UISettings","Duration",   500,   INIFileName));
    w->SetAlpha(GetPrivateProfileInt("UISettings",      "Alpha",       255,   INIFileName));
    w->SetFadeToAlpha(GetPrivateProfileInt("UISettings","FadeToAlpha", 255,   INIFileName));
    w->SetBGType(GetPrivateProfileInt("UISettings",     "BGType",      1,     INIFileName));
    ARGBCOLOR argb{};
    argb.A = GetPrivateProfileInt("UISettings", "BGTint.alpha", 255, INIFileName);
    argb.R = GetPrivateProfileInt("UISettings", "BGTint.red",   255, INIFileName);
    argb.G = GetPrivateProfileInt("UISettings", "BGTint.green", 255, INIFileName);
    argb.B = GetPrivateProfileInt("UISettings", "BGTint.blue",  255, INIFileName);
    w->SetBGColor(argb.ARGB);
    w->SetWindowText(&GetPrivateProfileString("UISettings", "WindowTitle", "MQ2 MyButton Window", INIFileName)[0]);
    w->Show(GetPrivateProfileBool("UISettings", "ShowWindow", true, INIFileName));
    w->UpdateLayout();
    if (w->bFullyScreenClipped)
        WriteChatf("MQ2MyButtons: window is off screen.");
}

static void WriteWindowINI(CSidlScreenWnd* w)
{
    WritePrivateProfileString("UISettings", "WindowTitle",  w->GetWindowText().c_str(),          INIFileName);
    WritePrivateProfileBool(  "UISettings", "Locked",       w->IsLocked(),                       INIFileName);
    WritePrivateProfileBool(  "UISettings", "Fades",        w->GetFades(),                       INIFileName);
    WritePrivateProfileInt(   "UISettings", "Delay",        w->GetFadeDelay(),                   INIFileName);
    WritePrivateProfileInt(   "UISettings", "Duration",     w->GetFadeDuration(),                INIFileName);
    WritePrivateProfileInt(   "UISettings", "Alpha",        w->GetAlpha(),                       INIFileName);
    WritePrivateProfileInt(   "UISettings", "FadeToAlpha",  w->GetFadeToAlpha(),                 INIFileName);
    WritePrivateProfileInt(   "UISettings", "BGType",       w->GetBGType(),                      INIFileName);
    ARGBCOLOR argb{};
    argb.ARGB = w->GetBGColor();
    WritePrivateProfileInt("UISettings", "BGTint.alpha", argb.A, INIFileName);
    WritePrivateProfileInt("UISettings", "BGTint.red",   argb.R, INIFileName);
    WritePrivateProfileInt("UISettings", "BGTint.green", argb.G, INIFileName);
    WritePrivateProfileInt("UISettings", "BGTint.blue",  argb.B, INIFileName);
    WritePrivateProfileBool("UISettings", "ShowWindow",   w->IsVisible(),                        INIFileName);
    WritePrivateProfileInt("Location", "Top",    w->GetLocation().top,    INIFileName);
    WritePrivateProfileInt("Location", "Bottom", w->GetLocation().bottom, INIFileName);
    WritePrivateProfileInt("Location", "Left",   w->GetLocation().left,   INIFileName);
    WritePrivateProfileInt("Location", "Right",  w->GetLocation().right,  INIFileName);
}

static void DestroyButtonWindow()
{
    if (MyBtnWnd) {
        WriteWindowINI(MyBtnWnd);
        delete MyBtnWnd;
        MyBtnWnd = nullptr;
    }
}

// -------------------------------------------------------------------------
// UI regeneration - called when settings change count or size
// -------------------------------------------------------------------------
static void RegenerateUI()
{
    DestroyButtonWindow();
    RemoveXMLFile(XML_FILE);
    if (GenerateXMLFile())
        AddXMLFile(XML_FILE);
    else
        MBError("Failed to regenerate " + std::string(XML_FILE));
}

// -------------------------------------------------------------------------
// ImGui settings panel (MQ menu -> plugins/MyButtons)
// -------------------------------------------------------------------------
static void MyButtonsSettingsPanel()
{
    const bool dirty = (g_numButtons != g_xmlNum || g_buttonWidth != g_xmlWidth || g_buttonHeight != g_xmlHeight);

    ImGui::SetNextItemWidth(200);
    ImGui::SliderInt("Button Count", &g_numButtons, 10, MAX_BUTTONS);
    ImGui::SameLine();
    mq::imgui::HelpMarker("Number of buttons (1-60). Requires 'Generate XML' to apply.\n\nINISetting: [Settings] MaxButtons");

    ImGui::SetNextItemWidth(200);
    ImGui::SliderInt("Button Width (px)", &g_buttonWidth, 16, 80);
    ImGui::SameLine();
    mq::imgui::HelpMarker("Button display width in pixels (16-80). Requires 'Generate XML' to apply.\n\nINISetting: [Settings] ButtonWidth");

    ImGui::SetNextItemWidth(200);
    ImGui::SliderInt("Button Height (px)", &g_buttonHeight, 16, 40);
    ImGui::SameLine();
    mq::imgui::HelpMarker("Button display height in pixels (16-40). Requires 'Generate XML' to apply.\n\nINISetting: [Settings] ButtonHeight");

    ImGui::Separator();

    if (dirty)
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.6f, 0.3f, 0.0f, 1.0f));
    if (ImGui::Button("Generate XML")) {
        WritePrivateProfileInt("Settings", "MaxButtons",   g_numButtons,   INIFileName);
        WritePrivateProfileInt("Settings", "ButtonWidth",  g_buttonWidth,  INIFileName);
        WritePrivateProfileInt("Settings", "ButtonHeight", g_buttonHeight, INIFileName);
        RegenerateUI();
        MBLog("XML regenerated: " + std::to_string(g_numButtons) + " buttons at "
              + std::to_string(g_buttonWidth) + "x" + std::to_string(g_buttonHeight) + "px.");
    }
    if (dirty)
        ImGui::PopStyleColor();
    ImGui::SameLine();
    mq::imgui::HelpMarker("Regenerates the UI XML with current settings and saves them to INI.\nRequired after changing button count or dimensions.");

    if (dirty) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.6f, 0.0f, 1.0f));
        ImGui::TextWrapped("WARNING: Button count or dimensions have changed. Current button labels and commands "
                           "will not match the active window until 'Generate XML' is clicked.");
        ImGui::PopStyleColor();
    }

    ImGui::Separator();

    if (ImGui::Button("Reload Buttons from INI")) {
        LoadButtonData();
        if (MyBtnWnd) MyBtnWnd->SetButtonInfo();
        MBLog("Buttons reloaded from INI.");
    }
    ImGui::SameLine();
    if (ImGui::Button("Show Button List")) {
        for (int i = 1; i <= g_numButtons; i++) {
            if (g_buttons[i].command[0])
                MBLog("[" + std::to_string(i) + "] " + g_buttons[i].label + " -> " + g_buttons[i].command);
        }
    }

    ImGui::Separator();
    ImGui::Text("Broadcast");
    ImGui::SameLine();
    mq::imgui::HelpMarker(
        "When enabled, saving a button edit sends a reload command to all other clients in the zone.\n"
        "Requires DanNet or EQBC to be loaded on all clients.\n\n"
        "Multi-line commands: use Enter in the button editor. Lines beginning with # are comments.");
    if (ImGui::Checkbox("Broadcast button changes to other clients", &g_broadcastEnabled))
        WritePrivateProfileBool("Settings", "BroadcastEnabled", g_broadcastEnabled, INIFileName);
    if (g_broadcastEnabled) {
        ImGui::Indent();
        bool changed = ImGui::RadioButton("DanNet (/dgze)", &g_broadcastMethod, 0);
        ImGui::SameLine();
        changed |= ImGui::RadioButton("EQBC (/bca)", &g_broadcastMethod, 1);
        if (changed)
            WritePrivateProfileInt("Settings", "BroadcastMethod", g_broadcastMethod, INIFileName);
        ImGui::Unindent();
    }
}

// -------------------------------------------------------------------------
// Command handler
// -------------------------------------------------------------------------
PLUGIN_API void MyButtonsCommand(SPAWNINFO* pSpawn, char* szLine)
{
    char szParam[MAX_STRING] = {};
    GetArg(szParam, szLine, 1);

    if (szParam[0] == '\0') {
        if (MyBtnWnd) MyBtnWnd->Show(!MyBtnWnd->IsVisible());
        return;
    }
    if (ci_equals(szParam, "help") || !strcmp(szParam, "?")) {
        MBLog("========== MyButtons Help ===========");
        MBLog("Edit MQ2MyButtons.ini to configure buttons. Right-click any button to edit in-game.");
        MBLog("TLOs: \at${MyButtons.label[N]}\ao, \at${MyButtons.cmd[N]}");
        MBLog("Commands: on | off | reload | show | <ButtonNumber>");
        MBLog("Settings panel available in MQ menu -> plugins/MyButtons");
        MBLog("--- Token syntax in commands ---");
        MBLog("\at^{expr}\ao  expands MQ data \aylocally\ao before the command is sent.");
        MBLog("\at${expr}\ao  passed through intact (parsed by receiver, e.g. via /noparse /bcaa).");
        MBLog("Example: \at/noparse /bcaa //if ( ${Raid.MA[1]} == ^{Me.Name} ) /nav id ^{Me.ID}");
        return;
    }
    if (ci_equals(szParam, "on"))                                { if (MyBtnWnd) MyBtnWnd->Show(true);  return; }
    if (ci_equals(szParam, "off") || ci_equals(szParam, "hide")) { if (MyBtnWnd) MyBtnWnd->Show(false); return; }
    if (ci_equals(szParam, "reload")) {
        LoadButtonData();
        if (MyBtnWnd) MyBtnWnd->SetButtonInfo();
        MBLog("Reloaded from INI.");
        return;
    }
    if (ci_equals(szParam, "show")) {
        for (int i = 1; i <= g_numButtons; i++) {
            if (g_buttons[i].command[0])
                MBLog("[" + std::to_string(i) + "] " + g_buttons[i].label + " -> " + g_buttons[i].command);
        }
        return;
    }

    const int i = GetIntFromString(szParam, 0);
    if (i >= 1 && i <= g_numButtons) {
        if (g_buttons[i].command[0])
            DoCommand(pSpawn, g_buttons[i].command);
        else
            MBError("No command set for button " + std::to_string(i));
    } else {
        MBError("Invalid: " + std::string(szParam));
    }
}

// -------------------------------------------------------------------------
// TLO
// -------------------------------------------------------------------------
bool MQ2MyBtnData(const char* szIndex, MQTypeVar& Dest);

class MQ2MyButtonsType : public MQ2Type
{
public:
    enum Members { Label, CMD };

    MQ2MyButtonsType() : MQ2Type("MyButtons")
    {
        TypeMember(Label);
        AddMember(Label, "label");
        TypeMember(CMD);
        AddMember(CMD, "cmd");
        AddMember(CMD, "Cmd");
    }

    bool GetMember(MQVarPtr VarPtr, const char* Member, char* Index, MQTypeVar& Dest) override
    {
        const auto* pMember = MQ2MyButtonsType::FindMember(Member);
        if (!pMember) return false;

        const int i = GetIntFromString(Index, 0);
        if (i < 1 || i > g_numButtons) {
            strcpy_s(DataTypeTemp, "InvalidButton");
        } else {
            switch ((Members)pMember->ID) {
                case Label: strcpy_s(DataTypeTemp, g_buttons[i].label);   break;
                case CMD:   strcpy_s(DataTypeTemp, g_buttons[i].command); break;
                default:    return false;
            }
        }
        Dest.Type = mq::datatypes::pStringType;
        Dest.Ptr  = &DataTypeTemp[0];
        return true;
    }
};

bool MQ2MyBtnData(const char* szIndex, MQTypeVar& Dest)
{
    Dest.DWord = 1;
    Dest.Type  = pMyButtonsType;
    return true;
}

// -------------------------------------------------------------------------
// In-game button editor (ImGui overlay, opened by right-clicking a button)
// -------------------------------------------------------------------------
PLUGIN_API void OnUpdateImGui()
{
    if (g_editingButton <= 0) return;

    ImGui::SetNextWindowSize(ImVec2(500, 380), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(400, 200), ImGuiCond_FirstUseEver);
    bool open = true;
    const std::string title = "Edit Button " + std::to_string(g_editingButton) + "##MBEdit";
    if (ImGui::Begin(title.c_str(), &open, ImGuiWindowFlags_NoCollapse)) {
        ImGui::Text("Label");
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputText("##label", g_editLabelBuf, sizeof(g_editLabelBuf));

        ImGui::Text("Command");
        ImGui::SameLine();
        mq::imgui::HelpMarker(
            "One command per line. Lines starting with # are comments.\n\n"
            "Token syntax:\n"
            "  ^{expr}  expands MQ data on THIS client before sending\n"
            "  ${expr}  passed through intact (each receiver parses their own)\n\n"
            "Example multi-line:\n"
            "  # pause macro on all toons\n"
            "  /noparse /bcaa //docommand /${Me.Class.ShortName} pause 1\n"
            "  # navigate all zone-mates to me\n"
            "  /dgex /nav id ^{Me.ID}");
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextMultiline("##cmd", g_editCmdBuf, sizeof(g_editCmdBuf),
                                  ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 8));

        ImGui::ColorEdit3("Color", g_editColor);

        // Reorder controls
        static int s_swapTarget  = 0;
        static int s_lastEditBtn = 0;
        if (g_editingButton != s_lastEditBtn) { s_swapTarget = 0; s_lastEditBtn = g_editingButton; }
        const int cur = g_editingButton;

        const auto writeBtn = [&](int i) {
            const std::string sec = "Button" + std::to_string(i);
            WritePrivateProfileString(sec.c_str(), "Label",   g_buttons[i].label,   INIFileName);
            WritePrivateProfileString(sec.c_str(), "Command", g_buttons[i].command, INIFileName);
            WritePrivateProfileInt(   sec.c_str(), "Red",     g_buttons[i].r,       INIFileName);
            WritePrivateProfileInt(   sec.c_str(), "Green",   g_buttons[i].g,       INIFileName);
            WritePrivateProfileInt(   sec.c_str(), "Blue",    g_buttons[i].b,       INIFileName);
        };
        const auto doSwap = [&](int a, int b) {
            std::swap(g_buttons[a], g_buttons[b]);
            writeBtn(a); writeBtn(b);
            if (MyBtnWnd) MyBtnWnd->SetButtonInfo();
            if (g_broadcastEnabled) {
                char bcCmd[MAX_STRING];
                if (g_broadcastMethod == 0) sprintf_s(bcCmd, "/dgex /mybuttons reload");
                else                        sprintf_s(bcCmd, "/bca //mybuttons reload");
                DoCommand(pCharSpawn, bcCmd);
            }
            g_editingButton = 0;
        };

        ImGui::Spacing();
        ImGui::BeginDisabled(cur <= 1);
        if (ImGui::ArrowButton("##moveup", ImGuiDir_Up))     doSwap(cur, cur - 1);
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(cur >= g_numButtons);
        if (ImGui::ArrowButton("##movedown", ImGuiDir_Down)) doSwap(cur, cur + 1);
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextDisabled("move");
        ImGui::SameLine();

        char swapPreview[MAX_STRING];
        if (s_swapTarget < 1 || s_swapTarget > g_numButtons || s_swapTarget == cur)
            strcpy_s(swapPreview, "-- swap with --");
        else
            sprintf_s(swapPreview, "[%d] %s", s_swapTarget,
                      g_buttons[s_swapTarget].label[0] ? g_buttons[s_swapTarget].label : "(empty)");

        ImGui::SetNextItemWidth(200);
        if (ImGui::BeginCombo("##swaptgt", swapPreview)) {
            for (int j = 1; j <= g_numButtons; j++) {
                if (j == cur) continue;
                char item[MAX_STRING];
                sprintf_s(item, "[%d] %s", j, g_buttons[j].label[0] ? g_buttons[j].label : "(empty)");
                if (ImGui::Selectable(item, s_swapTarget == j)) s_swapTarget = j;
                if (s_swapTarget == j) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(s_swapTarget < 1 || s_swapTarget > g_numButtons || s_swapTarget == cur);
        if (ImGui::Button("Swap##swapbtn")) doSwap(cur, s_swapTarget);
        ImGui::EndDisabled();

        ImGui::Separator();
        if (ImGui::Button("Save", ImVec2(80, 0))) {
            const int n = g_editingButton;
            const std::string sec = "Button" + std::to_string(n);
            char storageBuf[MAX_STRING];
            DisplayToStorage(g_editCmdBuf, storageBuf, sizeof(storageBuf));
            strcpy_s(g_buttons[n].label,   g_editLabelBuf);
            strcpy_s(g_buttons[n].command, storageBuf);
            g_buttons[n].r = static_cast<int>(g_editColor[0] * 255.0f);
            g_buttons[n].g = static_cast<int>(g_editColor[1] * 255.0f);
            g_buttons[n].b = static_cast<int>(g_editColor[2] * 255.0f);
            WritePrivateProfileString(sec.c_str(), "Label",   g_buttons[n].label,   INIFileName);
            WritePrivateProfileString(sec.c_str(), "Command", g_buttons[n].command, INIFileName);
            WritePrivateProfileInt(   sec.c_str(), "Red",     g_buttons[n].r,       INIFileName);
            WritePrivateProfileInt(   sec.c_str(), "Green",   g_buttons[n].g,       INIFileName);
            WritePrivateProfileInt(   sec.c_str(), "Blue",    g_buttons[n].b,       INIFileName);
            if (MyBtnWnd) MyBtnWnd->SetButtonInfo();
            if (g_broadcastEnabled) {
                char bcCmd[MAX_STRING];
                if (g_broadcastMethod == 0)
                    sprintf_s(bcCmd, "/dgex /mybuttons reload");
                else
                    sprintf_s(bcCmd, "/bca //mybuttons reload");
                DoCommand(pCharSpawn, bcCmd);
            }
            g_editingButton = 0;
        }
        ImGui::SameLine();
        if (ImGui::Button("Clear", ImVec2(80, 0))) {
            g_editLabelBuf[0] = '\0';
            g_editCmdBuf[0]   = '\0';
            g_editColor[0] = 1.0f; g_editColor[1] = 1.0f; g_editColor[2] = 1.0f;
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(80, 0))) {
            g_editingButton = 0;
        }
    }
    if (!open) g_editingButton = 0;
    ImGui::End();
}

// -------------------------------------------------------------------------
// Plugin lifecycle
// -------------------------------------------------------------------------
PLUGIN_API void OnCleanUI()
{
    DestroyButtonWindow();
}

PLUGIN_API void OnPulse()
{
    if (GetGameState() != GAMESTATE_INGAME) return;

    if (!MyBtnWnd) {
        if (pSidlMgr->FindScreenPieceTemplate("MQMBButtonWnd")) {
            MyBtnWnd = new CHButWnd();
            ReadWindowINI(MyBtnWnd);
            MyBtnWnd->SetButtonInfo();
            WriteWindowINI(MyBtnWnd);
        }
    } else {
        static uint64_t nextLabelUpdate = 0;
        const uint64_t  now             = GetTickCount64();
        if (now >= nextLabelUpdate) {
            for (int i = 1; i <= g_numButtons; i++)
                MyBtnWnd->SetLabel(i);
            nextLabelUpdate = now + 30000;
        }
    }
}

PLUGIN_API void InitializePlugin()
{
    DebugSpewAlways("Initializing MQ2MyButtons");
    LoadButtonData();
    if (EnsureXMLFile()) {
        AddXMLFile(XML_FILE);
        AddCommand("/mybuttons", MyButtonsCommand);
        pMyButtonsType = new MQ2MyButtonsType;
        AddMQ2Data("MyButtons", MQ2MyBtnData);
        AddSettingsPanel("plugins/MyButtons", MyButtonsSettingsPanel);
    } else {
        MBError("Could not create " + std::string(XML_FILE) + ". Plugin disabled.");
    }
}

PLUGIN_API void ShutdownPlugin()
{
    DebugSpewAlways("Shutting down MQ2MyButtons");
    DestroyButtonWindow();
    RemoveCommand("/mybuttons");
    RemoveMQ2Data("MyButtons");
    RemoveXMLFile(XML_FILE);
    RemoveSettingsPanel("plugins/MyButtons");
    delete pMyButtonsType;
    pMyButtonsType = nullptr;
}
