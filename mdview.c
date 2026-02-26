/*
 * MDView v2.1 - Total Commander Lister Plugin for Markdown
 * =========================================================
 * Lightweight WLX plugin: built-in Markdown->HTML, embedded MSHTML, zero deps.
 *
 * Hotkeys:
 *   Ctrl+Plus/Minus/0  Zoom in / out / reset
 *   Ctrl+D             Toggle dark/light mode
 *   Ctrl+T             Toggle Table of Contents sidebar
 *   Ctrl+F             Find in page with highlighting
 *   Ctrl+P             Print document
 *   Ctrl+G             Go to top
 *   Escape             Close find bar / TOC / help
 *   F1                 Show keyboard shortcuts help
 *
 * (c) 2026 - MIT License
 */

#define COBJMACROS
#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0600

#include <windows.h>
#include <ole2.h>
#include <exdisp.h>
#include <mshtml.h>
#include <mshtmhst.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ── TC Lister Plugin Interface ──────────────────────────────────────── */

#define LISTPLUGIN_OK    0
#define LISTPLUGIN_ERROR 1

typedef struct {
    int   size;
    DWORD PluginInterfaceVersionLow;
    DWORD PluginInterfaceVersionHi;
    char  DefaultIniName[MAX_PATH];
} ListDefaultParamStruct;

static LRESULT CALLBACK ContainerWndProc(HWND, UINT, WPARAM, LPARAM);
static char* read_file_utf8(const char*);
static char* md_to_html(const char*);
static int   is_dark_theme(void);
static void  navigate_to_html(IWebBrowser2*, const char*);

static const wchar_t CLASS_NAME[] = L"MDViewWLXContainer";
static HINSTANCE g_hInstance = NULL;
static int g_classRegistered = 0;

typedef struct {
    IWebBrowser2* pBrowser;
    IOleObject*   pOleObj;
    HWND          hwndIEServer;  /* The actual IE rendering window */
    WNDPROC       origIEProc;    /* Original wndproc of IE Server */
} MDViewData;

/* Execute JavaScript on the browser document */
static void exec_js(IWebBrowser2* pB, const wchar_t* code) {
    if (!pB) return;
    IDispatch* pDisp = NULL;
    IWebBrowser2_get_Document(pB, &pDisp);
    if (!pDisp) return;
    IHTMLDocument2* pDoc = NULL;
    IDispatch_QueryInterface(pDisp, &IID_IHTMLDocument2, (void**)&pDoc);
    IDispatch_Release(pDisp);
    if (!pDoc) return;
    IHTMLWindow2* pWin = NULL;
    IHTMLDocument2_get_parentWindow(pDoc, &pWin);
    IHTMLDocument2_Release(pDoc);
    if (!pWin) return;
    BSTR bCode = SysAllocString(code);
    BSTR bLang = SysAllocString(L"JavaScript");
    VARIANT vResult;
    VariantInit(&vResult);
    IHTMLWindow2_execScript(pWin, bCode, bLang, &vResult);
    SysFreeString(bCode);
    SysFreeString(bLang);
    IHTMLWindow2_Release(pWin);
}

/* Subclass proc for the IE Server window - only intercepts our Ctrl+ hotkeys,
   everything else (PgUp, PgDn, arrows, Escape, etc.) passes through untouched */
static LRESULT CALLBACK IEServerSubclassProc(HWND hwnd, UINT msg, WPARAM wP, LPARAM lP) {
    /* Get our container's data via the stored property */
    MDViewData* d = (MDViewData*)GetPropW(hwnd, L"MDViewData");
    if (!d) return DefWindowProcW(hwnd, msg, wP, lP);

    if (msg == WM_KEYDOWN) {
        int ctrl = GetKeyState(VK_CONTROL) & 0x8000;
        if (ctrl) {
            switch (wP) {
            case VK_OEM_PLUS: case VK_ADD:
                exec_js(d->pBrowser, L"zi()"); return 0;
            case VK_OEM_MINUS: case VK_SUBTRACT:
                exec_js(d->pBrowser, L"zo()"); return 0;
            case '0': case VK_NUMPAD0:
                exec_js(d->pBrowser, L"zr()"); return 0;
            case 'D':
                exec_js(d->pBrowser, L"td()"); return 0;
            case 'T':
                exec_js(d->pBrowser, L"ttoc()"); return 0;
            case 'F':
                exec_js(d->pBrowser, L"sf()"); return 0;
            case 'P':
                exec_js(d->pBrowser, L"window.print()"); return 0;
            case 'G':
                exec_js(d->pBrowser, L"window.scrollTo(0,0)"); return 0;
            case 'L':
                exec_js(d->pBrowser, L"tl()"); return 0;
            case 'W':
                if (GetKeyState(VK_SHIFT) & 0x8000)
                    exec_js(d->pBrowser, L"cn()");
                else
                    exec_js(d->pBrowser, L"cw()");
                return 0;
            case VK_OEM_2:
                exec_js(d->pBrowser, L"th()"); return 0;
            }
        }
        if (wP == VK_F1) {
            exec_js(d->pBrowser, L"th()"); return 0;
        }
        /* Escape: the IE control eats it, so forward to TC's lister parent */
        if (wP == VK_ESCAPE) {
            HWND parent = GetParent(GetParent(GetParent(hwnd))); /* IE_Server -> Shell DocObj -> Shell Embed -> our container -> TC lister */
            if (!parent) parent = GetParent(GetParent(hwnd));
            /* Walk up to find TC's lister window and send Escape */
            HWND w = hwnd;
            while (w) {
                HWND p = GetParent(w);
                if (!p) break;
                /* Send to the topmost parent we can find */
                w = p;
            }
            if (w) PostMessageW(w, WM_KEYDOWN, VK_ESCAPE, lP);
            return 0;
        }
    }

    return CallWindowProcW(d->origIEProc, hwnd, msg, wP, lP);
}

/* ── Minimal COM Site Implementation ─────────────────────────────────── */

typedef struct SiteImpl {
    IOleClientSite    clientSite;
    IOleInPlaceSite   inPlaceSite;
    IOleInPlaceFrame  inPlaceFrame;
    IDocHostUIHandler docHostUI;
    LONG              refCount;
    HWND              hwndParent;
} SiteImpl;

#define SITE_FROM_CLIENT(p)  ((SiteImpl*)((char*)(p) - offsetof(SiteImpl, clientSite)))
#define SITE_FROM_INPLACE(p) ((SiteImpl*)((char*)(p) - offsetof(SiteImpl, inPlaceSite)))
#define SITE_FROM_FRAME(p)   ((SiteImpl*)((char*)(p) - offsetof(SiteImpl, inPlaceFrame)))
#define SITE_FROM_DOCHOST(p) ((SiteImpl*)((char*)(p) - offsetof(SiteImpl, docHostUI)))

/* IOleClientSite */
static HRESULT STDMETHODCALLTYPE CS_QI(IOleClientSite* This, REFIID riid, void** ppv) {
    SiteImpl* s = SITE_FROM_CLIENT(This);
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IOleClientSite)) *ppv = &s->clientSite;
    else if (IsEqualIID(riid, &IID_IOleInPlaceSite))  *ppv = &s->inPlaceSite;
    else if (IsEqualIID(riid, &IID_IOleInPlaceFrame) || IsEqualIID(riid, &IID_IOleWindow)) *ppv = &s->inPlaceFrame;
    else if (IsEqualIID(riid, &IID_IDocHostUIHandler)) *ppv = &s->docHostUI;
    else { *ppv = NULL; return E_NOINTERFACE; }
    InterlockedIncrement(&s->refCount); return S_OK;
}
static ULONG STDMETHODCALLTYPE CS_AddRef(IOleClientSite* This) { return InterlockedIncrement(&SITE_FROM_CLIENT(This)->refCount); }
static ULONG STDMETHODCALLTYPE CS_Release(IOleClientSite* This) { SiteImpl* s = SITE_FROM_CLIENT(This); LONG r = InterlockedDecrement(&s->refCount); if (r==0) free(s); return r; }
static HRESULT STDMETHODCALLTYPE CS_Save(IOleClientSite* This) { return S_OK; }
static HRESULT STDMETHODCALLTYPE CS_GetMoniker(IOleClientSite* This, DWORD a, DWORD b, IMoniker** c) { return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE CS_GetContainer(IOleClientSite* This, IOleContainer** c) { *c = NULL; return E_NOINTERFACE; }
static HRESULT STDMETHODCALLTYPE CS_ShowObj(IOleClientSite* This) { return S_OK; }
static HRESULT STDMETHODCALLTYPE CS_OnShow(IOleClientSite* This, BOOL f) { return S_OK; }
static HRESULT STDMETHODCALLTYPE CS_ReqLayout(IOleClientSite* This) { return E_NOTIMPL; }
static IOleClientSiteVtbl g_csVtbl = { CS_QI, CS_AddRef, CS_Release, CS_Save, CS_GetMoniker, CS_GetContainer, CS_ShowObj, CS_OnShow, CS_ReqLayout };

/* IOleInPlaceSite */
static HRESULT STDMETHODCALLTYPE IPS_QI(IOleInPlaceSite* This, REFIID riid, void** ppv) { return CS_QI(&SITE_FROM_INPLACE(This)->clientSite, riid, ppv); }
static ULONG STDMETHODCALLTYPE IPS_AddRef(IOleInPlaceSite* This) { return InterlockedIncrement(&SITE_FROM_INPLACE(This)->refCount); }
static ULONG STDMETHODCALLTYPE IPS_Release(IOleInPlaceSite* This) { SiteImpl* s = SITE_FROM_INPLACE(This); LONG r = InterlockedDecrement(&s->refCount); if(r==0) free(s); return r; }
static HRESULT STDMETHODCALLTYPE IPS_GetWindow(IOleInPlaceSite* This, HWND* h) { *h = SITE_FROM_INPLACE(This)->hwndParent; return S_OK; }
static HRESULT STDMETHODCALLTYPE IPS_CSHelp(IOleInPlaceSite* This, BOOL f) { return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE IPS_CanAct(IOleInPlaceSite* This) { return S_OK; }
static HRESULT STDMETHODCALLTYPE IPS_OnAct(IOleInPlaceSite* This) { return S_OK; }
static HRESULT STDMETHODCALLTYPE IPS_OnUI(IOleInPlaceSite* This) { return S_OK; }
static HRESULT STDMETHODCALLTYPE IPS_GetWinCtx(IOleInPlaceSite* This, IOleInPlaceFrame** ppF, IOleInPlaceUIWindow** ppD, LPRECT rP, LPRECT rC, LPOLEINPLACEFRAMEINFO fi) {
    SiteImpl* s = SITE_FROM_INPLACE(This); *ppF = (IOleInPlaceFrame*)&s->inPlaceFrame; InterlockedIncrement(&s->refCount);
    *ppD = NULL; GetClientRect(s->hwndParent, rP); *rC = *rP; fi->fMDIApp = FALSE; fi->hwndFrame = s->hwndParent; fi->haccel = NULL; fi->cAccelEntries = 0; return S_OK;
}
static HRESULT STDMETHODCALLTYPE IPS_Scroll(IOleInPlaceSite* This, SIZE s) { return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE IPS_UIDeact(IOleInPlaceSite* This, BOOL f) { return S_OK; }
static HRESULT STDMETHODCALLTYPE IPS_IPDeact(IOleInPlaceSite* This) { return S_OK; }
static HRESULT STDMETHODCALLTYPE IPS_Discard(IOleInPlaceSite* This) { return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE IPS_DeactUndo(IOleInPlaceSite* This) { return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE IPS_PosRect(IOleInPlaceSite* This, LPCRECT r) { return S_OK; }
static IOleInPlaceSiteVtbl g_ipsVtbl = { IPS_QI, IPS_AddRef, IPS_Release, IPS_GetWindow, IPS_CSHelp, IPS_CanAct, IPS_OnAct, IPS_OnUI, IPS_GetWinCtx, IPS_Scroll, IPS_UIDeact, IPS_IPDeact, IPS_Discard, IPS_DeactUndo, IPS_PosRect };

/* IOleInPlaceFrame */
static HRESULT STDMETHODCALLTYPE IPF_QI(IOleInPlaceFrame* This, REFIID riid, void** ppv) { return CS_QI(&SITE_FROM_FRAME(This)->clientSite, riid, ppv); }
static ULONG STDMETHODCALLTYPE IPF_AddRef(IOleInPlaceFrame* This) { return InterlockedIncrement(&SITE_FROM_FRAME(This)->refCount); }
static ULONG STDMETHODCALLTYPE IPF_Release(IOleInPlaceFrame* This) { SiteImpl* s = SITE_FROM_FRAME(This); LONG r = InterlockedDecrement(&s->refCount); if(r==0) free(s); return r; }
static HRESULT STDMETHODCALLTYPE IPF_GetWindow(IOleInPlaceFrame* This, HWND* h) { *h = SITE_FROM_FRAME(This)->hwndParent; return S_OK; }
static HRESULT STDMETHODCALLTYPE IPF_CSHelp(IOleInPlaceFrame* This, BOOL f) { return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE IPF_GetBorder(IOleInPlaceFrame* This, LPRECT r) { return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE IPF_ReqBorder(IOleInPlaceFrame* This, LPCBORDERWIDTHS b) { return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE IPF_SetBorder(IOleInPlaceFrame* This, LPCBORDERWIDTHS b) { return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE IPF_SetActive(IOleInPlaceFrame* This, IOleInPlaceActiveObject* a, LPCOLESTR s) { return S_OK; }
static HRESULT STDMETHODCALLTYPE IPF_InsMenus(IOleInPlaceFrame* This, HMENU h, LPOLEMENUGROUPWIDTHS w) { return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE IPF_SetMenu(IOleInPlaceFrame* This, HMENU h, HOLEMENU hm, HWND hw) { return S_OK; }
static HRESULT STDMETHODCALLTYPE IPF_RemMenus(IOleInPlaceFrame* This, HMENU h) { return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE IPF_SetStatus(IOleInPlaceFrame* This, LPCOLESTR t) { return S_OK; }
static HRESULT STDMETHODCALLTYPE IPF_EnableMod(IOleInPlaceFrame* This, BOOL f) { return S_OK; }
static HRESULT STDMETHODCALLTYPE IPF_TransAccel(IOleInPlaceFrame* This, LPMSG m, WORD w) { return E_NOTIMPL; }
static IOleInPlaceFrameVtbl g_ipfVtbl = { IPF_QI, IPF_AddRef, IPF_Release, IPF_GetWindow, IPF_CSHelp, IPF_GetBorder, IPF_ReqBorder, IPF_SetBorder, IPF_SetActive, IPF_InsMenus, IPF_SetMenu, IPF_RemMenus, IPF_SetStatus, IPF_EnableMod, IPF_TransAccel };

/* IDocHostUIHandler */
static HRESULT STDMETHODCALLTYPE DH_QI(IDocHostUIHandler* This, REFIID riid, void** ppv) { return CS_QI(&SITE_FROM_DOCHOST(This)->clientSite, riid, ppv); }
static ULONG STDMETHODCALLTYPE DH_AddRef(IDocHostUIHandler* This) { return InterlockedIncrement(&SITE_FROM_DOCHOST(This)->refCount); }
static ULONG STDMETHODCALLTYPE DH_Release(IDocHostUIHandler* This) { SiteImpl* s = SITE_FROM_DOCHOST(This); LONG r = InterlockedDecrement(&s->refCount); if(r==0) free(s); return r; }
static HRESULT STDMETHODCALLTYPE DH_CtxMenu(IDocHostUIHandler* This, DWORD id, POINT* pt, IUnknown* o, IDispatch* d) { return S_OK; }
static HRESULT STDMETHODCALLTYPE DH_GetHostInfo(IDocHostUIHandler* This, DOCHOSTUIINFO* p) { p->cbSize=sizeof(DOCHOSTUIINFO); p->dwFlags=DOCHOSTUIFLAG_NO3DBORDER|DOCHOSTUIFLAG_THEME; p->dwDoubleClick=DOCHOSTUIDBLCLK_DEFAULT; return S_OK; }
static HRESULT STDMETHODCALLTYPE DH_ShowUI(IDocHostUIHandler* This, DWORD a, IOleInPlaceActiveObject* b, IOleCommandTarget* c, IOleInPlaceFrame* d, IOleInPlaceUIWindow* e) { return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE DH_HideUI(IDocHostUIHandler* This) { return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE DH_UpdateUI(IDocHostUIHandler* This) { return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE DH_EnableMod(IDocHostUIHandler* This, BOOL f) { return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE DH_OnDocAct(IDocHostUIHandler* This, BOOL f) { return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE DH_OnFrmAct(IDocHostUIHandler* This, BOOL f) { return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE DH_Resize(IDocHostUIHandler* This, LPCRECT r, IOleInPlaceUIWindow* w, BOOL f) { return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE DH_TransAccel(IDocHostUIHandler* This, LPMSG m, const GUID* g, DWORD d) { return S_FALSE; }
static HRESULT STDMETHODCALLTYPE DH_OptKey(IDocHostUIHandler* This, LPOLESTR* p, DWORD d) { return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE DH_DropTgt(IDocHostUIHandler* This, IDropTarget* dt, IDropTarget** pdt) { return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE DH_GetExt(IDocHostUIHandler* This, IDispatch** ppd) { *ppd=NULL; return S_FALSE; }
static HRESULT STDMETHODCALLTYPE DH_TransUrl(IDocHostUIHandler* This, DWORD d, LPWSTR url, LPWSTR* purl) { return S_FALSE; }
static HRESULT STDMETHODCALLTYPE DH_FilterDO(IDocHostUIHandler* This, IDataObject* d, IDataObject** pd) { return S_FALSE; }
static IDocHostUIHandlerVtbl g_dhVtbl = { DH_QI, DH_AddRef, DH_Release, DH_CtxMenu, DH_GetHostInfo, DH_ShowUI, DH_HideUI, DH_UpdateUI, DH_EnableMod, DH_OnDocAct, DH_OnFrmAct, DH_Resize, DH_TransAccel, DH_OptKey, DH_DropTgt, DH_GetExt, DH_TransUrl, DH_FilterDO };

static SiteImpl* CreateSiteImpl(HWND hwnd) {
    SiteImpl* s = (SiteImpl*)calloc(1, sizeof(SiteImpl));
    if (!s) return NULL;
    s->clientSite.lpVtbl = &g_csVtbl; s->inPlaceSite.lpVtbl = &g_ipsVtbl;
    s->inPlaceFrame.lpVtbl = &g_ipfVtbl; s->docHostUI.lpVtbl = &g_dhVtbl;
    s->refCount = 1; s->hwndParent = hwnd;
    return s;
}

/* ── String Buffer ───────────────────────────────────────────────────── */

typedef struct { char* data; size_t len; size_t cap; } StrBuf;

static void sb_init(StrBuf* sb) { sb->cap=4096; sb->data=(char*)malloc(sb->cap); sb->data[0]='\0'; sb->len=0; }
static void sb_ensure(StrBuf* sb, size_t x) { while(sb->len+x+1>sb->cap){sb->cap*=2; sb->data=(char*)realloc(sb->data,sb->cap);} }
static void sb_append(StrBuf* sb, const char* s) { size_t n=strlen(s); sb_ensure(sb,n); memcpy(sb->data+sb->len,s,n); sb->len+=n; sb->data[sb->len]='\0'; }
static void sb_append_char(StrBuf* sb, char c) { sb_ensure(sb,1); sb->data[sb->len++]=c; sb->data[sb->len]='\0'; }
static void sb_append_esc(StrBuf* sb, const char* s, size_t n) {
    for(size_t i=0;i<n;i++) switch(s[i]){
        case '&': sb_append(sb,"&amp;"); break;
        case '<': sb_append(sb,"&lt;"); break;
        case '>': sb_append(sb,"&gt;"); break;
        case '"': sb_append(sb,"&quot;"); break;
        default:  sb_append_char(sb,s[i]); break;
    }
}

/* ── Markdown Inline Parser ──────────────────────────────────────────── */

static void parse_inline(StrBuf* sb, const char* t, size_t len) {
    size_t i = 0;
    while (i < len) {
        /* Backslash escape */
        if (t[i]=='\\' && i+1<len) {
            char nx=t[i+1];
            if(nx=='*'||nx=='_'||nx=='`'||nx=='['||nx==']'||nx=='('||nx==')'||nx=='#'||nx=='~'||nx=='!'||nx=='|'||nx=='\\'||nx=='-')
            { sb_append_esc(sb,&t[i+1],1); i+=2; continue; }
        }
        /* Line break (2+ trailing spaces + \n) */
        if (t[i]==' ' && i+1<len && t[i+1]==' ') {
            size_t j=i+2; while(j<len&&t[j]==' ')j++;
            if(j<len&&t[j]=='\n'){ sb_append(sb,"<br>\n"); i=j+1; continue; }
        }
        /* Inline code */
        if (t[i]=='`') {
            int tk=0; size_t st=i; while(i<len&&t[i]=='`'){tk++;i++;}
            size_t e=i; int found=0;
            while(e<=len-tk){
                if(t[e]=='`'){ int ct=0;size_t ce=e; while(ce<len&&t[ce]=='`'){ct++;ce++;}
                    if(ct==tk){ sb_append(sb,"<code>"); sb_append_esc(sb,t+i,e-i); sb_append(sb,"</code>"); i=ce; found=1; break; } e=ce;
                } else e++;
            }
            if(!found) sb_append_esc(sb,t+st,tk);
            continue;
        }
        /* Image ![alt](url) */
        if (t[i]=='!' && i+1<len && t[i+1]=='[') {
            size_t as=i+2,j=as; int d=1;
            while(j<len&&d>0){if(t[j]=='[')d++;else if(t[j]==']')d--;if(d>0)j++;}
            if(j<len&&j+1<len&&t[j+1]=='('){
                size_t us=j+2,ue=us; while(ue<len&&t[ue]!=')')ue++;
                if(ue<len){ sb_append(sb,"<img alt=\""); sb_append_esc(sb,t+as,j-as);
                    sb_append(sb,"\" src=\""); sb_append_esc(sb,t+us,ue-us);
                    sb_append(sb,"\" style=\"max-width:100%\">"); i=ue+1; continue; }
            }
        }
        /* Link [text](url) */
        if (t[i]=='[') {
            size_t ts=i+1,j=ts; int d=1;
            while(j<len&&d>0){if(t[j]=='[')d++;else if(t[j]==']')d--;if(d>0)j++;}
            if(j<len&&j+1<len&&t[j+1]=='('){
                size_t us=j+2,ue=us; while(ue<len&&t[ue]!=')')ue++;
                if(ue<len){ sb_append(sb,"<a href=\""); sb_append_esc(sb,t+us,ue-us);
                    sb_append(sb,"\">"); parse_inline(sb,t+ts,j-ts); sb_append(sb,"</a>"); i=ue+1; continue; }
            }
        }
        /* Strikethrough ~~text~~ */
        if (t[i]=='~'&&i+1<len&&t[i+1]=='~') {
            size_t s2=i+2,e2=s2; while(e2+1<len&&!(t[e2]=='~'&&t[e2+1]=='~'))e2++;
            if(e2+1<len){ sb_append(sb,"<del>"); parse_inline(sb,t+s2,e2-s2); sb_append(sb,"</del>"); i=e2+2; continue; }
        }
        /* Bold+Italic ***text*** */
        if ((t[i]=='*'||t[i]=='_')&&i+2<len&&t[i+1]==t[i]&&t[i+2]==t[i]) {
            char m=t[i]; size_t s3=i+3,e3=s3;
            while(e3+2<len&&!(t[e3]==m&&t[e3+1]==m&&t[e3+2]==m))e3++;
            if(e3+2<len){ sb_append(sb,"<strong><em>"); parse_inline(sb,t+s3,e3-s3); sb_append(sb,"</em></strong>"); i=e3+3; continue; }
        }
        /* Bold **text** */
        if ((t[i]=='*'||t[i]=='_')&&i+1<len&&t[i+1]==t[i]) {
            char m=t[i]; size_t s2=i+2,e2=s2;
            while(e2+1<len&&!(t[e2]==m&&t[e2+1]==m))e2++;
            if(e2+1<len&&e2>s2){ sb_append(sb,"<strong>"); parse_inline(sb,t+s2,e2-s2); sb_append(sb,"</strong>"); i=e2+2; continue; }
        }
        /* Italic *text* */
        if ((t[i]=='*'||t[i]=='_')&&i+1<len&&t[i+1]!=t[i]&&t[i+1]!=' ') {
            char m=t[i]; size_t s1=i+1,e1=s1; while(e1<len&&t[e1]!=m)e1++;
            if(e1<len&&e1>s1&&t[e1-1]!=' '){ sb_append(sb,"<em>"); parse_inline(sb,t+s1,e1-s1); sb_append(sb,"</em>"); i=e1+1; continue; }
        }
        /* Autolink bare URLs */
        if (i+8<len&&(strncmp(t+i,"https://",8)==0||strncmp(t+i,"http://",7)==0)) {
            size_t us=i; while(i<len&&t[i]!=' '&&t[i]!='\n'&&t[i]!='\r'&&t[i]!=')'&&t[i]!='>'&&t[i]!='"')i++;
            while(i>us&&(t[i-1]=='.'||t[i-1]==','||t[i-1]==';'))i--;
            sb_append(sb,"<a href=\""); sb_append_esc(sb,t+us,i-us); sb_append(sb,"\">"); sb_append_esc(sb,t+us,i-us); sb_append(sb,"</a>"); continue;
        }
        /* Plain char */
        sb_append_esc(sb,&t[i],1); i++;
    }
}

/* ── Markdown Block Parser Helpers ───────────────────────────────────── */

static int count_leading(const char* l, char c) { int n=0; while(l[n]==c)n++; return n; }
typedef struct { char** lines; int count; } Lines;
static Lines split_lines(const char* text) {
    Lines r; r.count=0; int cap=256; r.lines=(char**)malloc(cap*sizeof(char*));
    const char* p=text;
    while(*p){ const char* eol=p; while(*eol&&*eol!='\n')eol++;
        size_t ll=eol-p; if(ll>0&&p[ll-1]=='\r')ll--;
        char* line=(char*)malloc(ll+1); memcpy(line,p,ll); line[ll]='\0';
        if(r.count>=cap){cap*=2;r.lines=(char**)realloc(r.lines,cap*sizeof(char*));}
        r.lines[r.count++]=line; p=eol; if(*p=='\n')p++;
    } return r;
}
static void free_lines(Lines* l) { for(int i=0;i<l->count;i++) free(l->lines[i]); free(l->lines); }
static int is_hr(const char* l) { const char* p=l; while(*p==' ')p++; char c=*p; if(c!='-'&&c!='*'&&c!='_')return 0; int n=0; while(*p){if(*p==c)n++;else if(*p!=' ')return 0;p++;} return n>=3; }

static int is_table_sep(const char* l) {
    const char* p=l; while(*p==' ')p++; if(*p=='|')p++;
    int cells=0;
    while(*p){ while(*p==' ')p++; if(*p==':')p++; if(*p!='-')return 0; while(*p=='-')p++; if(*p==':')p++; while(*p==' ')p++; cells++; if(*p=='|'){p++;continue;} if(*p=='\0')break; return 0; }
    return cells>0;
}

static int parse_trow(const char* l, char cells[][1024], int mx) {
    const char* p=l; while(*p==' ')p++; if(*p=='|')p++;
    int nc=0;
    while(*p&&nc<mx){
        const char* s=p; int ic=0;
        while(*p){if(*p=='`')ic=!ic;if(*p=='\\'&&*(p+1)){p+=2;continue;}if(*p=='|'&&!ic)break;p++;}
        const char* e=p; while(s<e&&*s==' ')s++; while(e>s&&*(e-1)==' ')e--;
        size_t cl=e-s; if(cl>=1024)cl=1023; memcpy(cells[nc],s,cl); cells[nc][cl]='\0'; nc++;
        if(*p=='|')p++;
    }
    if(nc>0&&cells[nc-1][0]=='\0')nc--;
    return nc;
}

static void parse_talign(const char* l, char al[], int mx) {
    const char* p=l; while(*p==' ')p++; if(*p=='|')p++; int c=0;
    while(*p&&c<mx){ while(*p==' ')p++; int left=(*p==':');if(*p==':')p++; while(*p=='-')p++; int right=(*p==':');if(*p==':')p++; while(*p==' ')p++;
        if(left&&right)al[c]='c'; else if(right)al[c]='r'; else al[c]='l'; c++; if(*p=='|')p++;
    }
}

static int get_indent(const char* l) { int n=0; while(l[n]==' ')n++; if(l[n]=='\t')return n+4; return n; }
static int is_ul(const char* t) { return(t[0]=='-'||t[0]=='*'||t[0]=='+')&&t[1]==' '; }
static int is_ol(const char* t) { int i=0; while(t[i]>='0'&&t[i]<='9')i++; if(i==0||i>9)return 0; if((t[i]=='.'||t[i]==')')&&t[i+1]==' ')return i+2; return 0; }

/* ── Markdown Block Parser ───────────────────────────────────────────── */

static char* md_to_html(const char* markdown) {
    StrBuf sb; sb_init(&sb);
    Lines lines = split_lines(markdown);
    int i = 0;

    while (i < lines.count) {
        const char* line = lines.lines[i];
        int indent = get_indent(line);
        const char* tr = line + indent;

        if (tr[0]=='\0') { i++; continue; }

        /* Fenced code block */
        if (strncmp(tr,"```",3)==0 || strncmp(tr,"~~~",3)==0) {
            char fc=tr[0]; const char* lang=tr+3; while(*lang==' ')lang++;
            sb_append(&sb,"<pre><code");
            if(*lang){ sb_append(&sb," class=\"language-"); const char* le=lang; while(*le&&*le!=' '&&*le!='`'&&*le!='~')le++; sb_append_esc(&sb,lang,le-lang); sb_append(&sb,"\""); }
            sb_append(&sb,">"); i++;
            while(i<lines.count){ const char* cl=lines.lines[i]; const char* ct=cl; while(*ct==' ')ct++;
                if((fc=='`'&&strncmp(ct,"```",3)==0)||(fc=='~'&&strncmp(ct,"~~~",3)==0)){i++;break;}
                if(sb.data[sb.len-1]!='>') sb_append(&sb,"\n");
                sb_append_esc(&sb,cl,strlen(cl)); i++;
            }
            sb_append(&sb,"</code></pre>\n"); continue;
        }

        /* Indented code block */
        if (indent>=4 && !is_ul(tr) && !is_ol(tr)) {
            sb_append(&sb,"<pre><code>");
            while(i<lines.count){ const char* cl=lines.lines[i];
                if(cl[0]=='\0'){if(i+1<lines.count&&get_indent(lines.lines[i+1])>=4){sb_append(&sb,"\n");i++;continue;}break;}
                if(get_indent(cl)<4)break;
                if(sb.data[sb.len-1]!='>') sb_append(&sb,"\n");
                sb_append_esc(&sb,cl+4,strlen(cl+4)); i++;
            }
            sb_append(&sb,"</code></pre>\n"); continue;
        }

        /* ATX Headings with id for TOC */
        if (tr[0]=='#') {
            int lv=count_leading(tr,'#');
            if(lv>=1&&lv<=6&&tr[lv]==' '){
                const char* c=tr+lv+1; size_t cl=strlen(c);
                while(cl>0&&c[cl-1]=='#')cl--; while(cl>0&&c[cl-1]==' ')cl--;
                char tag[4]; sprintf(tag,"h%d",lv);
                char idnum[16]; sprintf(idnum,"%d",i);
                sb_append(&sb,"<"); sb_append(&sb,tag); sb_append(&sb," id=\"mdv-h");
                sb_append(&sb,idnum); sb_append(&sb,"\">");
                parse_inline(&sb,c,cl);
                sb_append(&sb,"</"); sb_append(&sb,tag); sb_append(&sb,">\n"); i++; continue;
            }
        }

        /* Setext headings */
        if (i+1<lines.count && tr[0]!='\0') {
            const char* nx=lines.lines[i+1]; while(*nx==' ')nx++; int nl=(int)strlen(nx);
            if(nl>=1){ int ae=1,ad=1; for(int j=0;j<nl;j++){if(nx[j]!='=')ae=0;if(nx[j]!='-')ad=0;}
                if(ae){ sb_append(&sb,"<h1>"); parse_inline(&sb,tr,strlen(tr)); sb_append(&sb,"</h1>\n"); i+=2; continue; }
                if(ad&&!is_hr(lines.lines[i+1])){ sb_append(&sb,"<h2>"); parse_inline(&sb,tr,strlen(tr)); sb_append(&sb,"</h2>\n"); i+=2; continue; }
            }
        }

        /* HR */
        if (is_hr(line)) { sb_append(&sb,"<hr>\n"); i++; continue; }

        /* Blockquote */
        if (tr[0]=='>'&&(tr[1]==' '||tr[1]=='\0')) {
            StrBuf bq; sb_init(&bq);
            while(i<lines.count){ const char* bl=lines.lines[i]; while(*bl==' ')bl++;
                if(bl[0]=='>'&&(bl[1]==' '||bl[1]=='\0')){if(bq.len>0)sb_append(&bq,"\n");sb_append(&bq,bl[1]==' '?bl+2:bl+1);i++;}
                else if(bl[0]=='\0')break; else{sb_append(&bq,"\n");sb_append(&bq,bl);i++;}
            }
            char* inner=md_to_html(bq.data);
            sb_append(&sb,"<blockquote>\n"); sb_append(&sb,inner); sb_append(&sb,"</blockquote>\n");
            free(inner); free(bq.data); continue;
        }

        /* Table */
        if (i+1<lines.count && is_table_sep(lines.lines[i+1])) {
            char cells[64][1024]; char al[64]; memset(al,'l',sizeof(al));
            int nc=parse_trow(line,cells,64); parse_talign(lines.lines[i+1],al,64);
            sb_append(&sb,"<table>\n<thead>\n<tr>\n");
            for(int c=0;c<nc;c++){
                sb_append(&sb,"<th"); if(al[c]=='c')sb_append(&sb," style=\"text-align:center\""); else if(al[c]=='r')sb_append(&sb," style=\"text-align:right\"");
                sb_append(&sb,">"); parse_inline(&sb,cells[c],strlen(cells[c])); sb_append(&sb,"</th>\n");
            }
            sb_append(&sb,"</tr>\n</thead>\n<tbody>\n"); i+=2;
            while(i<lines.count){ const char* rl=lines.lines[i]; while(*rl==' ')rl++;
                if(rl[0]=='\0'||!strchr(rl,'|'))break;
                int rc=parse_trow(lines.lines[i],cells,64); sb_append(&sb,"<tr>\n");
                for(int c=0;c<nc;c++){
                    sb_append(&sb,"<td"); if(al[c]=='c')sb_append(&sb," style=\"text-align:center\""); else if(al[c]=='r')sb_append(&sb," style=\"text-align:right\"");
                    sb_append(&sb,">"); if(c<rc)parse_inline(&sb,cells[c],strlen(cells[c])); sb_append(&sb,"</td>\n");
                }
                sb_append(&sb,"</tr>\n"); i++;
            }
            sb_append(&sb,"</tbody>\n</table>\n"); continue;
        }

        /* Lists */
        if (is_ul(tr) || is_ol(tr)) {
            int ordered=is_ol(tr); int bi=indent;
            sb_append(&sb,ordered?"<ol>\n":"<ul>\n");
            while(i<lines.count){
                const char* ll=lines.lines[i]; int li=get_indent(ll); const char* lt=ll+li;
                int iu=is_ul(lt)&&li<=bi+1; int om=is_ol(lt); int io=om&&li<=bi+1;
                if(lt[0]=='\0'){i++;continue;}
                if(!iu&&!io&&li<=bi)break;
                if(iu||io){
                    const char* ic=iu?lt+2:lt+om;
                    int task=0,chk=0;
                    if(strncmp(ic,"[ ] ",4)==0){task=1;ic+=4;}
                    else if(strncmp(ic,"[x] ",4)==0||strncmp(ic,"[X] ",4)==0){task=1;chk=1;ic+=4;}
                    sb_append(&sb,"<li>");
                    if(task) sb_append(&sb,chk?"<input type=\"checkbox\" checked disabled> ":"<input type=\"checkbox\" disabled> ");
                    parse_inline(&sb,ic,strlen(ic)); i++;
                    StrBuf nest; sb_init(&nest); int hn=0;
                    while(i<lines.count){ const char* nl=lines.lines[i]; int ni=get_indent(nl); const char* nt=nl+ni;
                        if(nt[0]=='\0'){if(i+1<lines.count&&get_indent(lines.lines[i+1])>bi+1){sb_append(&nest,"\n");i++;hn=1;continue;}break;}
                        if(ni>bi+1){if(nest.len>0)sb_append(&nest,"\n");sb_append(&nest,nl);hn=1;i++;}else break;
                    }
                    if(hn){char* nh=md_to_html(nest.data);sb_append(&sb,"\n");sb_append(&sb,nh);free(nh);}
                    free(nest.data); sb_append(&sb,"</li>\n");
                } else i++;
            }
            sb_append(&sb,ordered?"</ol>\n":"</ul>\n"); continue;
        }

        /* Paragraph */
        { StrBuf para; sb_init(&para);
            while(i<lines.count){
                const char* pl=lines.lines[i]; int pi=get_indent(pl); const char* pt=pl+pi;
                if(pt[0]=='\0')break; if(pt[0]=='#'&&pt[1]==' ')break; if(is_hr(pl))break;
                if(pt[0]=='>'&&(pt[1]==' '||pt[1]=='\0'))break;
                if(strncmp(pt,"```",3)==0||strncmp(pt,"~~~",3)==0)break;
                if(is_ul(pt)||is_ol(pt))break;
                if(i+1<lines.count&&is_table_sep(lines.lines[i+1]))break;
                if(para.len>0) sb_append(&para,"\n");
                sb_append(&para,tr); i++;
                if(i<lines.count){ const char* nx=lines.lines[i]; while(*nx==' ')nx++; int nl2=(int)strlen(nx);
                    if(nl2>=1){int ae=1,ad=1;for(int j=0;j<nl2;j++){if(nx[j]!='=')ae=0;if(nx[j]!='-')ad=0;}if(ae||ad)break;}
                    pi=get_indent(lines.lines[i]); tr=lines.lines[i]+pi;
                }
            }
            sb_append(&sb,"<p>"); parse_inline(&sb,para.data,para.len); sb_append(&sb,"</p>\n");
            free(para.data);
        }
    }
    free_lines(&lines);
    return sb.data;
}

/* ── Theme Detection ─────────────────────────────────────────────────── */

static int is_dark_theme(void) {
    HKEY hKey; DWORD val=1, sz=sizeof(DWORD);
    if(RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", 0, KEY_READ, &hKey)==ERROR_SUCCESS){
        RegQueryValueExW(hKey,L"AppsUseLightTheme",NULL,NULL,(LPBYTE)&val,&sz); RegCloseKey(hKey); return val==0;
    } return 0;
}

/* ── INI Settings Persistence ────────────────────────────────────────── */

static char g_iniPath[MAX_PATH] = {0};

typedef struct {
    int fontSize;    /* 9-30, default 19 */
    int isDark;      /* 0 or 1, -1 = auto */
    int maxWidth;    /* column width in px, 0 = no limit, default 0 */
    int lineNums;    /* 0 or 1 */
} MDVSettings;

static MDVSettings g_settings = { 19, -1, 960, 0 };

static void load_settings(void) {
    if (!g_iniPath[0]) return;
    g_settings.fontSize = GetPrivateProfileIntA("MDView", "FontSize", 19, g_iniPath);
    g_settings.isDark   = GetPrivateProfileIntA("MDView", "DarkMode", -1, g_iniPath);
    g_settings.maxWidth = GetPrivateProfileIntA("MDView", "MaxWidth", 0, g_iniPath);
    g_settings.lineNums = GetPrivateProfileIntA("MDView", "LineNumbers", 0, g_iniPath);
    /* Clamp */
    if (g_settings.fontSize < 9) g_settings.fontSize = 9;
    if (g_settings.fontSize > 30) g_settings.fontSize = 30;
    if (g_settings.maxWidth != 0 && g_settings.maxWidth < 400) g_settings.maxWidth = 400;
    if (g_settings.maxWidth > 9999) g_settings.maxWidth = 9999;
}

static void save_setting_int(const char* key, int val) {
    if (!g_iniPath[0]) return;
    char buf[16]; sprintf(buf, "%d", val);
    WritePrivateProfileStringA("MDView", key, buf, g_iniPath);
}

/* ── CSS ─────────────────────────────────────────────────────────────── */

static void build_css(StrBuf* sb) {
    sb_append(sb,
    "*{box-sizing:border-box}"
    "html{background:#fff;min-height:100%;width:100%}");

    /* Body — full viewport background */
    sb_append(sb, "body{font-family:'Segoe UI',Tahoma,Geneva,Verdana,sans-serif;");
    { char tmp[64]; sprintf(tmp, "font-size:%dpx;", g_settings.fontSize); sb_append(sb, tmp); }
    sb_append(sb, "line-height:1.7;color:#24292e;background:#fff;margin:0;padding:0;"
    "transition:background .2s,color .2s}");
    sb_append(sb, "body.dark{color:#d4d4d4;background:#1e1e1e}");

    /* Content container — centered, optional max-width */
    sb_append(sb, "#mdv-ct{margin:0 auto;padding:12px 32px 24px;");
    if (g_settings.maxWidth > 0) {
        char tmp[64]; sprintf(tmp, "max-width:%dpx;", g_settings.maxWidth); sb_append(sb, tmp);
    }
    sb_append(sb, "}");

    sb_append(sb,
    "h1,h2,h3,h4,h5,h6{color:#1a1a1a;margin-top:1.4em;margin-bottom:.6em;font-weight:600}"
    "body.dark h1,body.dark h2,body.dark h3,body.dark h4,body.dark h5,body.dark h6{color:#e0e0e0}"
    "#mdv-ct>:first-child{margin-top:0}"
    "h1{font-size:2em;padding-bottom:.3em;border-bottom:1px solid #e1e4e8}"
    "h2{font-size:1.5em;padding-bottom:.25em;border-bottom:1px solid #e1e4e8}"
    "body.dark h1,body.dark h2{border-bottom-color:#444}"
    "h3{font-size:1.25em}"
    "a{color:#0366d6;text-decoration:none}a:hover{text-decoration:underline}"
    "body.dark a{color:#569cd6}"
    "code{font-family:Consolas,'Courier New',monospace;background:#f6f8fa;"
    "padding:2px 6px;border-radius:3px;font-size:.9em;color:#d73a49}"
    "body.dark code{background:#2d2d2d;color:#ce9178}"
    "pre{background:#f6f8fa;border:1px solid #e1e4e8;border-radius:6px;"
    "padding:16px;overflow-x:auto;line-height:1.5;position:relative;white-space:pre-wrap;word-wrap:break-word}"
    "body.dark pre{background:#2d2d2d;border-color:#404040}"
    "pre code{background:none;padding:0;color:#24292e;white-space:pre-wrap;word-wrap:break-word}"
    "body.dark pre code{color:#d4d4d4}"
    "blockquote{margin:.8em 0;padding:.5em 1em;border-left:4px solid #0366d6;"
    "background:#f6f8fa;color:#6a737d}"
    "body.dark blockquote{border-left-color:#569cd6;background:#252526;color:#aaa}"
    "blockquote p{margin:.4em 0}"
    "table{border-collapse:collapse;width:100%;margin:1em 0}"
    "th,td{border:1px solid #dfe2e5;padding:8px 12px}"
    "body.dark th,body.dark td{border-color:#444}"
    "th{background:#f6f8fa;font-weight:600}"
    "body.dark th{background:#2d2d2d}"
    "tr:nth-child(even){background:#f9f9f9}"
    "body.dark tr:nth-child(even){background:#252526}"
    "hr{border:none;border-top:1px solid #e1e4e8;margin:1.5em 0}"
    "body.dark hr{border-top-color:#444}"
    "img{max-width:100%;border-radius:4px}"
    "ul,ol{padding-left:2em}li{margin:.3em 0}"
    "input[type=checkbox]{margin-right:6px}"
    "del{color:#999}body.dark del{color:#888}"

    /* Line numbers */
    "pre.ln{padding-left:0}pre.ln code{display:block}"
    ".ln-wrap{display:table;width:100%}"
    ".ln-nums{display:table-cell;width:1px;padding:0 12px;text-align:right;"
    "color:#6a737d;border-right:1px solid #e1e4e8;user-select:none;"
    "-webkit-user-select:none;white-space:pre;vertical-align:top;"
    "font-family:Consolas,'Courier New',monospace;font-size:.9em;line-height:1.5}"
    "body.dark .ln-nums{color:#aaa;border-right-color:#404040}"
    ".ln-code{display:table-cell;padding:0 16px;white-space:pre-wrap;word-wrap:break-word;"
    "vertical-align:top;overflow-x:auto;font-family:Consolas,'Courier New',monospace;font-size:.9em;line-height:1.5}"

    /* Expand/collapse */
    ".mdv-collapsible{max-height:400px;overflow:hidden;position:relative}"
    ".mdv-collapsible.expanded{max-height:none}"
    ".mdv-expand-btn{display:block;text-align:center;padding:8px;margin-top:4px;cursor:pointer;"
    "color:#0366d6;font-size:13px;font-family:'Segoe UI',sans-serif;"
    "background:linear-gradient(transparent,#f6f8fa 60%);border:none;position:relative}"
    "body.dark .mdv-expand-btn{color:#569cd6;background:linear-gradient(transparent,#2d2d2d 60%)}"
    ".mdv-collapse-fade{position:absolute;bottom:0;left:0;right:0;height:60px;"
    "background:linear-gradient(rgba(246,248,250,0),#f6f8fa);pointer-events:none}"
    "body.dark .mdv-collapse-fade{background:linear-gradient(rgba(45,45,45,0),#2d2d2d)}"

    /* Syntax highlighting */
    ".sh-kw{color:#d73a49}body.dark .sh-kw{color:#569cd6}"
    ".sh-str{color:#032f62}body.dark .sh-str{color:#ce9178}"
    ".sh-num{color:#005cc5}body.dark .sh-num{color:#b5cea8}"
    ".sh-cm{color:#6a737d;font-style:italic}body.dark .sh-cm{color:#6a9955;font-style:italic}"
    ".sh-fn{color:#6f42c1}body.dark .sh-fn{color:#dcdcaa}"
    ".sh-op{color:#d73a49}body.dark .sh-op{color:#d4d4d4}"
    ".sh-type{color:#22863a}body.dark .sh-type{color:#4ec9b0}"
    ".sh-tag{color:#22863a}body.dark .sh-tag{color:#569cd6}"
    ".sh-attr{color:#6f42c1}body.dark .sh-attr{color:#9cdcfe}"
    ".sh-val{color:#032f62}body.dark .sh-val{color:#ce9178}"

    /* Find bar */
    "#mdv-fb{display:none;position:fixed;top:0;left:0;right:0;z-index:10000;"
    "background:#f0f0f0;border-bottom:1px solid #ccc;"
    "padding:6px 12px;font:13px 'Segoe UI',sans-serif;box-shadow:0 2px 8px rgba(0,0,0,.15)}"
    "body.dark #mdv-fb{background:#2d2d2d;border-bottom-color:#555}"
    "#mdv-fb.on{display:flex;align-items:center;gap:8px}"
    "#mdv-fi{padding:4px 8px;border:1px solid #bbb;border-radius:3px;"
    "font-size:13px;width:280px;outline:none;background:#fff;color:#24292e}"
    "body.dark #mdv-fi{background:#1e1e1e;border-color:#555;color:#d4d4d4}"
    "#mdv-fi:focus{border-color:#0366d6}"
    "body.dark #mdv-fi:focus{border-color:#569cd6}"
    "#mdv-fc{color:#6a737d;min-width:70px;font-size:12px}"
    ".fb{padding:3px 10px;border:1px solid #bbb;border-radius:3px;"
    "background:#e0e0e0;color:#24292e;cursor:pointer;font-size:13px}"
    "body.dark .fb{background:#404040;border-color:#555;color:#d4d4d4}"
    ".fb:hover{background:#d0d0d0}body.dark .fb:hover{background:#505050}"
    ".hl{background:#fff59d;color:#000;border-radius:2px}"
    ".hl-a{background:#ff9800;color:#000;font-weight:bold}"
    "body.dark .hl{background:#6b5b00}body.dark .hl-a{background:#b37400}"

    /* TOC */
    "#mdv-toc{display:none;position:fixed;top:0;right:0;bottom:0;width:280px;z-index:9999;"
    "background:#f6f8fa;border-left:1px solid #e1e4e8;"
    "overflow-y:auto;padding:16px 0;font:13px 'Segoe UI',sans-serif;"
    "box-shadow:-2px 0 12px rgba(0,0,0,.1)}"
    "body.dark #mdv-toc{background:#252526;border-left-color:#404040}"
    "#mdv-toc.on{display:block}"
    "#mdv-toc-t{padding:0 16px 12px;font-size:14px;font-weight:700;color:#1a1a1a;"
    "border-bottom:1px solid #e1e4e8;margin-bottom:8px}"
    "body.dark #mdv-toc-t{color:#e0e0e0;border-bottom-color:#404040}"
    ".ti{display:block;padding:4px 16px;color:#24292e;text-decoration:none;cursor:pointer;border-left:3px solid transparent}"
    "body.dark .ti{color:#d4d4d4}"
    ".ti:hover{background:#ebeef1;border-left-color:#0366d6}"
    "body.dark .ti:hover{background:#2d2d2d;border-left-color:#569cd6}"
    ".t1{font-weight:600}.t2{padding-left:28px}.t3{padding-left:40px;font-size:12px}"
    ".t4{padding-left:52px;font-size:12px;color:#6a737d}body.dark .t4{color:#aaa}"

    /* Toast */
    "#mdv-toast{display:none;position:fixed;bottom:20px;left:50%;transform:translateX(-50%);"
    "background:#333;color:#fff;padding:8px 20px;"
    "border-radius:6px;font:13px 'Segoe UI',sans-serif;"
    "box-shadow:0 4px 12px rgba(0,0,0,.3);z-index:10001;opacity:0;transition:opacity .3s}"
    "body.dark #mdv-toast{background:#555}"
    "#mdv-toast.on{display:block;opacity:1}"

    /* Progress */
    "#mdv-prog{position:fixed;top:0;left:0;height:3px;z-index:10002;"
    "background:#0366d6;width:0;transition:width .1s}"
    "body.dark #mdv-prog{background:#569cd6}"

    /* Help overlay */
    "#mdv-help{display:none;position:fixed;top:50%;left:50%;transform:translate(-50%,-50%);"
    "background:#ffffff;border:1px solid #ccc;border-radius:12px;"
    "padding:28px 36px;z-index:10003;box-shadow:0 12px 48px rgba(0,0,0,.35);"
    "font:16px 'Segoe UI',sans-serif;min-width:440px;max-width:520px;color:#24292e}"
    "body.dark #mdv-help{background:#2d2d2d;border-color:#555;color:#d4d4d4}"
    "#mdv-help.on{display:block}"
    "#mdv-help h3{margin:0 0 16px;color:#1a1a1a;font-size:20px;font-weight:700;"
    "padding-bottom:12px;border-bottom:1px solid #ddd}"
    "body.dark #mdv-help h3{color:#e0e0e0;border-bottom-color:#444}"
    ".hrow{display:flex;justify-content:space-between;align-items:center;padding:7px 0}"
    ".hrow span:first-child{color:#24292e;font-size:15px}"
    "body.dark .hrow span:first-child{color:#d4d4d4}"
    ".hkeys{display:flex;gap:5px;align-items:center}"
    ".kc{display:inline-block;font-family:Consolas,'Courier New',monospace;font-size:13px;"
    "background:#f0f0f0;color:#333;padding:4px 9px;border-radius:5px;"
    "border:1px solid #ccc;border-bottom-width:2px;"
    "min-width:26px;text-align:center;line-height:1.3;"
    "box-shadow:0 1px 0 #bbb}"
    "body.dark .kc{background:#404040;color:#ddd;border-color:#555;box-shadow:0 1px 0 #333}"
    ".kc-plus{color:#999;font-size:12px;padding:0 2px}"
    "body.dark .kc-plus{color:#888}"
    ".help-sep{height:1px;background:#e0e0e0;margin:8px 0}"
    "body.dark .help-sep{background:#444}"
    ".help-foot{font-size:13px;color:#999;text-align:center;margin-top:14px}"
    "body.dark .help-foot{color:#777}"
    );
}

/* ── JavaScript ──────────────────────────────────────────────────────── */

static void build_js(StrBuf* sb) {
    /* Initial values from settings */
    char init[128];
    sprintf(init, "<script>var fs=%d,mw=%d,ln=%d,tt=null;",
            g_settings.fontSize, g_settings.maxWidth, g_settings.lineNums);
    sb_append(sb, init);

    sb_append(sb,
    /* Toast */
    "function toast(m){var t=document.getElementById('mdv-toast');t.innerText=m;t.className='on';"
    "if(tt)clearTimeout(tt);tt=setTimeout(function(){t.className=''},1500)}"

    /* Zoom */
    "function zi(){fs=Math.min(fs+1,30);af()}"
    "function zo(){fs=Math.max(fs-1,9);af()}"
    "function zr(){fs=19;af()}"
    "function af(){document.body.style.fontSize=fs+'px';toast('Font: '+fs+'px')}"

    /* Theme toggle */
    "function td(){var b=document.body,h=document.documentElement;"
    "if(b.className.indexOf('dark')>=0){b.className=b.className.replace('dark','').replace(/^\\s+|\\s+$/g,'');"
    "h.style.background='#fff';toast('Light mode')}"
    "else{b.className=(b.className?b.className+' ':'')+'dark';"
    "h.style.background='#1e1e1e';toast('Dark mode')}}"

    /* Column width */
    "function cw(){if(mw===0)mw=800;mw=Math.min(mw+80,9999);aw()}"
    "function cn(){if(mw===0)return;mw=mw-80;if(mw<400){mw=0;document.getElementById('mdv-ct').style.maxWidth='none';toast('Width: full');return;}aw()}"
    "function aw(){document.getElementById('mdv-ct').style.maxWidth=mw+'px';toast('Width: '+mw+'px')}"

    /* Line numbers toggle */
    "function tl(){"
    "ln=ln?0:1;var ps=document.querySelectorAll('pre');"
    "for(var i=0;i<ps.length;i++){"
    "var p=ps[i];"
    "if(ln&&!p._lnDone){"
    "  var code=p.getElementsByTagName('code')[0];if(!code)continue;"
    "  var htm=code.innerHTML;"
    /* Count lines by splitting on newlines in the HTML */
    "  var tmp=htm.replace(/<br\\s*\\/?>/gi,'\\n');"
    "  var lines=tmp.split('\\n');"
    "  while(lines.length>1&&lines[lines.length-1].replace(/\\s|&nbsp;/g,'')==='')lines.pop();"
    "  var nums='';"
    "  for(var j=0;j<lines.length;j++){nums+=(j+1)+'\\n';}"
    "  var wrap=document.createElement('div');wrap.className='ln-wrap';"
    "  var nd=document.createElement('div');nd.className='ln-nums';"
    "  nd.style.whiteSpace='pre';nd.appendChild(document.createTextNode(nums));"
    "  var cd=document.createElement('div');cd.className='ln-code';cd.innerHTML=htm;"
    "  wrap.appendChild(nd);wrap.appendChild(cd);"
    "  code.innerHTML='';code.appendChild(wrap);"
    "  p.className=(p.className?p.className+' ':'')+'ln';p._lnDone=1;"
    "}else if(!ln&&p._lnDone){"
    "  var cd2=p.querySelector('.ln-code');if(cd2){var code2=p.getElementsByTagName('code')[0];"
    "  if(code2){code2.innerHTML=cd2.innerHTML;}}"
    "  p.className=p.className.replace(/\\bln\\b/g,'').replace(/^\\s+|\\s+$/g,'');p._lnDone=0;"
    "}}"
    "toast(ln?'Line numbers ON':'Line numbers OFF')}"

    /* TOC */
    "function btoc(){var toc=document.getElementById('mdv-toc');"
    "var hs=document.querySelectorAll('h1[id],h2[id],h3[id],h4[id]');"
    "var old=toc.querySelectorAll('.ti');for(var i=0;i<old.length;i++)old[i].parentNode.removeChild(old[i]);"
    "for(var i=0;i<hs.length;i++){var h=hs[i],a=document.createElement('a');"
    "a.className='ti t'+h.tagName.charAt(1);a.innerText=h.innerText;a.href='#'+h.id;"
    "a.onclick=(function(id){return function(e){e.preventDefault?e.preventDefault():e.returnValue=false;var el=document.getElementById(id);if(el)el.scrollIntoView()}})(h.id);"
    "toc.appendChild(a)}}"
    "function ttoc(){var toc=document.getElementById('mdv-toc');"
    "if(toc.className.indexOf('on')>=0){toc.className='';document.body.style.marginRight='0'}"
    "else{btoc();toc.className='on';document.body.style.marginRight='280px'}}"

    /* Find */
    "var fm=[],fi=-1;"
    "function cf(){var ms=document.querySelectorAll('.hl');for(var i=0;i<ms.length;i++){"
    "var m=ms[i],p=m.parentNode;p.replaceChild(document.createTextNode(m.innerText),m);p.normalize()}"
    "fm=[];fi=-1;document.getElementById('mdv-fc').innerText=''}"

    "function df(txt){cf();if(!txt)return;var lo=txt.toLowerCase();"
    "var ct=document.getElementById('mdv-ct');"
    "var w=document.createTreeWalker(ct,4,null,false),ns=[];"
    "while(w.nextNode()){var n=w.currentNode;"
    "if(n.parentNode&&n.parentNode.tagName==='SCRIPT')continue;"
    "if(n.nodeValue&&n.nodeValue.toLowerCase().indexOf(lo)>=0)ns.push(n)}"
    "for(var ni=0;ni<ns.length;ni++){var nd=ns[ni],v=nd.nodeValue,f=document.createDocumentFragment(),"
    "idx=0,vl=v.toLowerCase(),pos;"
    "while((pos=vl.indexOf(lo,idx))>=0){"
    "if(pos>idx)f.appendChild(document.createTextNode(v.substring(idx,pos)));"
    "var sp=document.createElement('span');sp.className='hl';"
    "sp.appendChild(document.createTextNode(v.substring(pos,pos+txt.length)));f.appendChild(sp);idx=pos+txt.length}"
    "if(idx<v.length)f.appendChild(document.createTextNode(v.substring(idx)));nd.parentNode.replaceChild(f,nd)}"
    "fm=document.querySelectorAll('.hl');fi=fm.length>0?0:-1;ufh()}"

    "function ufh(){for(var i=0;i<fm.length;i++)fm[i].className='hl';"
    "if(fi>=0&&fi<fm.length){fm[fi].className='hl hl-a';fm[fi].scrollIntoView()}"
    "var c=document.getElementById('mdv-fc');"
    "if(fm.length>0)c.innerText=(fi+1)+' of '+fm.length;"
    "else c.innerText=document.getElementById('mdv-fi').value?'No matches':''}"

    "function fn(){if(fm.length===0)return;fi=(fi+1)%fm.length;ufh()}"
    "function fp(){if(fm.length===0)return;fi=(fi-1+fm.length)%fm.length;ufh()}"
    "function sf(){var b=document.getElementById('mdv-fb');b.className='on';"
    "var inp=document.getElementById('mdv-fi');inp.focus();inp.select()}"
    "function hf(){document.getElementById('mdv-fb').className='';cf()}"

    /* Help */
    "function th(){var h=document.getElementById('mdv-help');h.className=h.className==='on'?'':'on'}"

    /* Progress */
    "function up(){var b=document.getElementById('mdv-prog'),d=document.body,"
    "st=d.scrollTop||document.body.scrollTop,sh=d.scrollHeight-d.clientHeight;"
    "if(sh>0)b.style.width=(st/sh*100)+'%';else b.style.width='0'}"
    "window.onscroll=up;"

    /* Syntax highlighting — regex-based, applied once on load */
    "function shAll(){"
    "var pres=document.querySelectorAll('pre code[class]');"
    "for(var i=0;i<pres.length;i++){var el=pres[i],cls=el.className||'';"
    "var lang=cls.replace('language-','');"
    "if(!lang)continue;"
    "var h=el.innerHTML;"

    /* Comments */
    "if(lang==='html'||lang==='xml'){"
    "h=h.replace(/(&lt;!--[\\s\\S]*?--&gt;)/g,'<span class=\"sh-cm\">$1</span>');"
    "}else{"
    "h=h.replace(/((?:^|\\n)\\s*#[^\\n]*)/g,'<span class=\"sh-cm\">$1</span>');"  /* # comments for python/bash/ruby */
    "h=h.replace(/(\\/{2}[^\\n]*)/g,'<span class=\"sh-cm\">$1</span>');"          /* // comments */
    "h=h.replace(/(--[^\\n]*)/g,'<span class=\"sh-cm\">$1</span>');"              /* -- sql comments */
    "h=h.replace(/(\\/\\*[\\s\\S]*?\\*\\/)/g,'<span class=\"sh-cm\">$1</span>');" /* block comments */
    "}"

    /* Strings */
    "h=h.replace(/(&quot;(?:[^&]|&(?!quot;))*?&quot;)/g,'<span class=\"sh-str\">$1</span>');"
    "h=h.replace(/('(?:[^'\\\\]|\\\\.)*?')/g,'<span class=\"sh-str\">$1</span>');"
    "h=h.replace(/(`(?:[^`\\\\]|\\\\.)*?`)/g,'<span class=\"sh-str\">$1</span>');"

    /* Numbers */
    "h=h.replace(/\\b(\\d+\\.?\\d*(?:e[+-]?\\d+)?|0x[0-9a-fA-F]+)\\b/g,'<span class=\"sh-num\">$1</span>');"

    /* HTML/XML tags */
    "if(lang==='html'||lang==='xml'){"
    "h=h.replace(/(&lt;\\/?)([a-zA-Z][a-zA-Z0-9]*)/g,'$1<span class=\"sh-tag\">$2</span>');"
    "h=h.replace(/\\s([a-zA-Z-]+)(=)/g,' <span class=\"sh-attr\">$1</span>$2');"
    "}else{"

    /* Keywords per language family */
    "var kws='';"
    "if(lang==='js'||lang==='javascript'||lang==='typescript'||lang==='ts')"
    "kws='\\\\b(var|let|const|function|return|if|else|for|while|do|switch|case|break|continue|new|this|class|extends|import|export|from|default|try|catch|finally|throw|typeof|instanceof|async|await|yield|of|in|null|undefined|true|false)\\\\b';"
    "else if(lang==='python'||lang==='py')"
    "kws='\\\\b(def|class|return|if|elif|else|for|while|break|continue|import|from|as|try|except|finally|raise|with|yield|lambda|pass|and|or|not|in|is|None|True|False|self|print|global|nonlocal|assert|del)\\\\b';"
    "else if(lang==='c'||lang==='cpp'||lang==='csharp'||lang==='cs'||lang==='java'||lang==='rust'||lang==='go')"
    "kws='\\\\b(int|char|float|double|void|long|short|unsigned|signed|struct|enum|union|typedef|sizeof|static|extern|const|volatile|register|auto|return|if|else|for|while|do|switch|case|break|continue|goto|default|class|public|private|protected|virtual|override|new|delete|this|true|false|null|NULL|nullptr|fn|let|mut|pub|impl|use|mod|match|loop|async|await|func|package|import|var|type|interface|defer|range|select|chan)\\\\b';"
    "else if(lang==='sql')"
    "kws='\\\\b(SELECT|FROM|WHERE|INSERT|UPDATE|DELETE|CREATE|DROP|ALTER|TABLE|INDEX|JOIN|LEFT|RIGHT|INNER|OUTER|ON|AND|OR|NOT|IN|IS|NULL|AS|ORDER|BY|GROUP|HAVING|LIMIT|OFFSET|UNION|ALL|DISTINCT|INTO|VALUES|SET|BEGIN|COMMIT|ROLLBACK|EXISTS|BETWEEN|LIKE|COUNT|SUM|AVG|MAX|MIN|select|from|where|insert|update|delete|create|drop|alter|table|join|left|right|inner|outer|on|and|or|not|in|is|null|as|order|by|group|having|limit)\\\\b';"
    "else if(lang==='bash'||lang==='sh'||lang==='shell'||lang==='zsh')"
    "kws='\\\\b(if|then|else|elif|fi|for|while|do|done|case|esac|in|function|return|local|export|source|echo|exit|cd|ls|grep|sed|awk|cat|rm|mkdir|cp|mv|chmod|chown|sudo|apt|yum|pip|npm)\\\\b';"
    "else if(lang==='css'||lang==='scss'||lang==='less')"
    "kws='\\\\b(color|background|margin|padding|border|font|display|position|width|height|top|left|right|bottom|flex|grid|none|block|inline|relative|absolute|fixed|inherit|auto|important|solid|transparent)\\\\b';"
    "else if(lang==='php')"
    "kws='\\\\b(function|return|if|else|elseif|for|foreach|while|do|switch|case|break|continue|class|public|private|protected|static|new|echo|print|null|true|false|array|isset|empty|unset|require|include|use|namespace|try|catch|finally|throw|var)\\\\b';"
    "if(kws){var re=new RegExp(kws,'g');h=h.replace(re,'<span class=\"sh-kw\">$1</span>');}"

    /* Function calls: word followed by ( */
    "h=h.replace(/\\b([a-zA-Z_][a-zA-Z0-9_]*)\\s*\\(/g,'<span class=\"sh-fn\">$1</span>(');"
    "}"

    "el.innerHTML=h;}}"

    /* Expand/collapse for long blocks */
    "function initCollapse(){"
    "var blocks=document.querySelectorAll('pre,blockquote');"
    "for(var i=0;i<blocks.length;i++){"
    "var b=blocks[i];if(b.scrollHeight>420){"
    "b.className=(b.className?b.className+' ':'')+'mdv-collapsible';"
    "var fade=document.createElement('div');fade.className='mdv-collapse-fade';b.appendChild(fade);"
    "var btn=document.createElement('button');btn.className='mdv-expand-btn';"
    "btn.innerText='\\u25BC Show more';btn.onclick=(function(bl,fd,bt){"
    "return function(){if(bl.className.indexOf('expanded')>=0){"
    "bl.className=bl.className.replace(' expanded','');bt.innerText='\\u25BC Show more';fd.style.display=''}"
    "else{bl.className+=' expanded';bt.innerText='\\u25B2 Show less';fd.style.display='none'}}"
    "})(b,fade,btn);"
    "b.parentNode.insertBefore(btn,b.nextSibling);"
    "}}}"

    /* Keyboard handler (backup — primary interception is via IE subclass) */
    "function pd(e){if(e.preventDefault)e.preventDefault();else e.returnValue=false}"
    "document.onkeydown=function(e){"
    "e=e||window.event;var c=e.ctrlKey||e.metaKey,k=e.keyCode;"
    "if(c&&(k===187||k===107)){pd(e);zi();return false}"
    "if(c&&(k===189||k===109)){pd(e);zo();return false}"
    "if(c&&(k===48||k===96)){pd(e);zr();return false}"
    "if(c&&k===68){pd(e);td();return false}"
    "if(c&&k===84){pd(e);ttoc();return false}"
    "if(c&&k===70){pd(e);sf();return false}"
    "if(c&&k===80){pd(e);window.print();return false}"
    "if(c&&k===71){pd(e);window.scrollTo(0,0);return false}"
    "if(c&&k===76){pd(e);tl();return false}"
    "if(c&&k===87){pd(e);if(e.shiftKey)cn();else cw();return false}"
    "if(k===112||(c&&k===191)){pd(e);th();return false}"
    "if(k===27){hf();document.getElementById('mdv-toc').className='';document.body.style.marginRight='0';"
    "document.getElementById('mdv-help').className='';return false}"
    "if(k===13&&document.activeElement===document.getElementById('mdv-fi')){"
    "pd(e);if(e.shiftKey)fp();else fn();return false}"
    "};"

    /* Init */
    "window.onload=function(){"
    "var inp=document.getElementById('mdv-fi');if(inp){"
    "inp.onkeyup=function(){var s=this;if(s._db)clearTimeout(s._db);"
    "s._db=setTimeout(function(){df(s.value)},200)}}"
    "shAll();initCollapse();"
    "if(ln)tl();"  /* apply line numbers if saved */
    "up()};"
    "</script>");
}

/* ── HTML UI elements ────────────────────────────────────────────────── */

static const char* get_ui(void) {
    return
    "<div id=\"mdv-prog\"></div>"
    "<div id=\"mdv-fb\">"
    "<input id=\"mdv-fi\" type=\"text\" placeholder=\"Find in document...\" autocomplete=\"off\">"
    "<span id=\"mdv-fc\"></span>"
    "<button class=\"fb\" onclick=\"fp()\">&laquo;</button>"
    "<button class=\"fb\" onclick=\"fn()\">&raquo;</button>"
    "<button class=\"fb\" onclick=\"hf()\">&times;</button>"
    "</div>"
    "<div id=\"mdv-toc\"><div id=\"mdv-toc-t\">Table of Contents</div></div>"
    "<div id=\"mdv-toast\"></div>"
    "<div id=\"mdv-help\">"
    "<h3>MDView Keyboard Shortcuts</h3>"

    "<div class=\"hrow\"><span>Zoom in</span><span class=\"hkeys\"><span class=\"kc\">Ctrl</span><span class=\"kc-plus\">+</span><span class=\"kc\">+</span></span></div>"
    "<div class=\"hrow\"><span>Zoom out</span><span class=\"hkeys\"><span class=\"kc\">Ctrl</span><span class=\"kc-plus\">+</span><span class=\"kc\">&minus;</span></span></div>"
    "<div class=\"hrow\"><span>Reset zoom</span><span class=\"hkeys\"><span class=\"kc\">Ctrl</span><span class=\"kc-plus\">+</span><span class=\"kc\">0</span></span></div>"
    "<div class=\"help-sep\"></div>"

    "<div class=\"hrow\"><span>Widen columns</span><span class=\"hkeys\"><span class=\"kc\">Ctrl</span><span class=\"kc-plus\">+</span><span class=\"kc\">W</span></span></div>"
    "<div class=\"hrow\"><span>Narrow columns</span><span class=\"hkeys\"><span class=\"kc\">Ctrl</span><span class=\"kc-plus\">+</span><span class=\"kc\">Shift</span><span class=\"kc-plus\">+</span><span class=\"kc\">W</span></span></div>"
    "<div class=\"help-sep\"></div>"

    "<div class=\"hrow\"><span>Toggle dark / light</span><span class=\"hkeys\"><span class=\"kc\">Ctrl</span><span class=\"kc-plus\">+</span><span class=\"kc\">D</span></span></div>"
    "<div class=\"hrow\"><span>Line numbers</span><span class=\"hkeys\"><span class=\"kc\">Ctrl</span><span class=\"kc-plus\">+</span><span class=\"kc\">L</span></span></div>"
    "<div class=\"help-sep\"></div>"

    "<div class=\"hrow\"><span>Table of Contents</span><span class=\"hkeys\"><span class=\"kc\">Ctrl</span><span class=\"kc-plus\">+</span><span class=\"kc\">T</span></span></div>"
    "<div class=\"hrow\"><span>Find in page</span><span class=\"hkeys\"><span class=\"kc\">Ctrl</span><span class=\"kc-plus\">+</span><span class=\"kc\">F</span></span></div>"
    "<div class=\"hrow\"><span>Print</span><span class=\"hkeys\"><span class=\"kc\">Ctrl</span><span class=\"kc-plus\">+</span><span class=\"kc\">P</span></span></div>"
    "<div class=\"hrow\"><span>Go to top</span><span class=\"hkeys\"><span class=\"kc\">Ctrl</span><span class=\"kc-plus\">+</span><span class=\"kc\">G</span></span></div>"
    "<div class=\"help-sep\"></div>"

    "<div class=\"hrow\"><span>Close viewer</span><span class=\"hkeys\"><span class=\"kc\">Esc</span></span></div>"
    "<div class=\"hrow\"><span>This help</span><span class=\"hkeys\"><span class=\"kc\">F1</span></span></div>"

    "<div class=\"help-foot\">MDView v2.1 &middot; Settings auto-saved &middot; Press Esc to close</div>"
    "</div>";
}

/* ── File Reading ────────────────────────────────────────────────────── */

static char* read_file_utf8(const char* fn) {
    FILE* f=fopen(fn,"rb"); if(!f)return NULL;
    fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET);
    char* buf=(char*)malloc(sz+1); if(!buf){fclose(f);return NULL;}
    fread(buf,1,sz,f); buf[sz]='\0'; fclose(f);
    if(sz>=3&&(unsigned char)buf[0]==0xEF&&(unsigned char)buf[1]==0xBB&&(unsigned char)buf[2]==0xBF)
        memmove(buf,buf+3,sz-2);
    return buf;
}

/* ── WebBrowser Control ──────────────────────────────────────────────── */

#ifndef READYSTATE_LOADED
#define READYSTATE_LOADED      2
#define READYSTATE_INTERACTIVE 3
#define READYSTATE_COMPLETE    4
#endif

static HRESULT create_browser(HWND hwnd, IWebBrowser2** ppB, IOleObject** ppO, SiteImpl** ppS) {
    CLSID clsid; CLSIDFromString(L"{8856F961-340A-11D0-A96B-00C04FD705A2}",&clsid);
    IOleObject* pO=NULL;
    HRESULT hr=CoCreateInstance(&clsid,NULL,CLSCTX_INPROC_SERVER|CLSCTX_LOCAL_SERVER,&IID_IOleObject,(void**)&pO);
    if(FAILED(hr))return hr;
    SiteImpl* site=CreateSiteImpl(hwnd); *ppS=site;
    IOleObject_SetClientSite(pO,(IOleClientSite*)&site->clientSite);
    RECT rc; GetClientRect(hwnd,&rc);
    IOleObject_DoVerb(pO,OLEIVERB_INPLACEACTIVATE,NULL,(IOleClientSite*)&site->clientSite,0,hwnd,&rc);
    IWebBrowser2* pB=NULL;
    hr=IOleObject_QueryInterface(pO,&IID_IWebBrowser2,(void**)&pB);
    if(FAILED(hr)){IOleObject_Release(pO);return hr;}
    *ppB=pB; *ppO=pO; return S_OK;
}

static void navigate_to_html(IWebBrowser2* pB, const char* html) {
    VARIANT ve; VariantInit(&ve);
    BSTR url=SysAllocString(L"about:blank");
    IWebBrowser2_Navigate(pB,url,&ve,&ve,&ve,&ve); SysFreeString(url);

    READYSTATE rs; int to=100;
    do{ MSG msg; while(PeekMessageW(&msg,NULL,0,0,PM_REMOVE)){TranslateMessage(&msg);DispatchMessageW(&msg);}
        IWebBrowser2_get_ReadyState(pB,&rs);
        if(rs!=READYSTATE_LOADED&&rs!=READYSTATE_INTERACTIVE&&rs!=READYSTATE_COMPLETE)Sleep(10);
    }while(rs!=READYSTATE_LOADED&&rs!=READYSTATE_INTERACTIVE&&rs!=READYSTATE_COMPLETE&&--to>0);

    IDispatch* pD=NULL; IWebBrowser2_get_Document(pB,&pD); if(!pD)return;
    IHTMLDocument2* pDoc=NULL; IDispatch_QueryInterface(pD,&IID_IHTMLDocument2,(void**)&pDoc); IDispatch_Release(pD);
    if(!pDoc)return;

    int wl=MultiByteToWideChar(CP_UTF8,0,html,-1,NULL,0);
    wchar_t* wh=(wchar_t*)malloc(wl*sizeof(wchar_t));
    MultiByteToWideChar(CP_UTF8,0,html,-1,wh,wl);
    BSTR bh=SysAllocString(wh); free(wh);

    SAFEARRAY* sa=SafeArrayCreateVector(VT_VARIANT,0,1);
    VARIANT* pv; SafeArrayAccessData(sa,(void**)&pv);
    pv->vt=VT_BSTR; pv->bstrVal=bh; SafeArrayUnaccessData(sa);
    IHTMLDocument2_write(pDoc,sa); IHTMLDocument2_close(pDoc);
    SafeArrayDestroy(sa); IHTMLDocument2_Release(pDoc);
}

/* ── Window Procedure ────────────────────────────────────────────────── */

static BOOL CALLBACK FindIEServerProc(HWND hwnd, LPARAM lParam) {
    wchar_t cls[64]; GetClassNameW(hwnd, cls, 64);
    if (wcscmp(cls, L"Internet Explorer_Server") == 0) { *(HWND*)lParam = hwnd; return FALSE; }
    return TRUE;
}

/* Read current JS state and save to INI */
static void save_current_settings(IWebBrowser2* pB) {
    if (!pB || !g_iniPath[0]) return;
    /* Read values from JS globals via execScript + document.title hack */
    /* We set document.title to a serialized string, then read it */
    exec_js(pB, L"document.title=''+fs+','+mw+','+ln+','+(document.body.className.indexOf('dark')>=0?1:0)");
    /* Now read the title back */
    IDispatch* pDisp = NULL;
    IWebBrowser2_get_Document(pB, &pDisp);
    if (!pDisp) return;
    IHTMLDocument2* pDoc = NULL;
    IDispatch_QueryInterface(pDisp, &IID_IHTMLDocument2, (void**)&pDoc);
    IDispatch_Release(pDisp);
    if (!pDoc) return;
    BSTR bTitle = NULL;
    IHTMLDocument2_get_title(pDoc, &bTitle);
    IHTMLDocument2_Release(pDoc);
    if (bTitle) {
        char title[128];
        WideCharToMultiByte(CP_UTF8, 0, bTitle, -1, title, sizeof(title), NULL, NULL);
        SysFreeString(bTitle);
        int rfs=19, rmw=0, rln=0, rdk=0;
        sscanf(title, "%d,%d,%d,%d", &rfs, &rmw, &rln, &rdk);
        save_setting_int("FontSize", rfs);
        save_setting_int("MaxWidth", rmw);
        save_setting_int("LineNumbers", rln);
        save_setting_int("DarkMode", rdk);
    }
}

static LRESULT CALLBACK ContainerWndProc(HWND hwnd, UINT msg, WPARAM wP, LPARAM lP) {
    MDViewData* d=(MDViewData*)GetWindowLongPtrW(hwnd,GWLP_USERDATA);
    switch(msg){
    case WM_SIZE:
        if(d&&d->pBrowser){
            RECT rc; GetClientRect(hwnd,&rc);
            /* Resize via IWebBrowser2 */
            IWebBrowser2_put_Left(d->pBrowser, 0);
            IWebBrowser2_put_Top(d->pBrowser, 0);
            IWebBrowser2_put_Width(d->pBrowser, rc.right);
            IWebBrowser2_put_Height(d->pBrowser, rc.bottom);
            /* Also resize via IOleInPlaceObject for OLE containers that need it */
            if(d->pOleObj){
                IOleInPlaceObject* pIPO = NULL;
                IOleObject_QueryInterface(d->pOleObj, &IID_IOleInPlaceObject, (void**)&pIPO);
                if(pIPO){
                    IOleInPlaceObject_SetObjectRects(pIPO, &rc, &rc);
                    IOleInPlaceObject_Release(pIPO);
                }
            }
            /* Force all child windows to match our size */
            HWND child = GetWindow(hwnd, GW_CHILD);
            while(child){
                MoveWindow(child, 0, 0, rc.right, rc.bottom, TRUE);
                child = GetWindow(child, GW_CHILD);
            }
        }
        return 0;
    case WM_SETFOCUS:
        if(d&&d->hwndIEServer) SetFocus(d->hwndIEServer);
        return 0;
    case WM_DESTROY:
        if(d){
            /* Save settings to INI before closing */
            save_current_settings(d->pBrowser);
            if(d->hwndIEServer && d->origIEProc){
                SetWindowLongPtrW(d->hwndIEServer, GWLP_WNDPROC, (LONG_PTR)d->origIEProc);
                RemovePropW(d->hwndIEServer, L"MDViewData");
            }
            if(d->pBrowser) IWebBrowser2_Release(d->pBrowser);
            if(d->pOleObj){ IOleObject_Close(d->pOleObj,OLECLOSE_NOSAVE); IOleObject_Release(d->pOleObj); }
            free(d); SetWindowLongPtrW(hwnd,GWLP_USERDATA,0);
        }
        return 0;
    }
    return DefWindowProcW(hwnd,msg,wP,lP);
}

/* ── DLL Entry ───────────────────────────────────────────────────────── */

BOOL WINAPI DllMain(HINSTANCE hInst, DWORD reason, LPVOID res) {
    if(reason==DLL_PROCESS_ATTACH){g_hInstance=hInst;DisableThreadLibraryCalls(hInst);}
    return TRUE;
}

/* Force IE11 edge mode for embedded WebBrowser control */
static void ensure_ie11_emulation(void) {
    static int done = 0;
    if (done) return; done = 1;
    wchar_t path[MAX_PATH]; GetModuleFileNameW(NULL, path, MAX_PATH);
    wchar_t* exe = wcsrchr(path, L'\\'); exe = exe ? exe + 1 : path;
    HKEY hKey;
    if (RegCreateKeyExW(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Internet Explorer\\Main\\FeatureControl\\FEATURE_BROWSER_EMULATION",
            0, NULL, 0, KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS) {
        DWORD val = 11001;
        RegSetValueExW(hKey, exe, 0, REG_DWORD, (LPBYTE)&val, sizeof(val));
        RegCloseKey(hKey);
    }
}

/* ── TC Lister Plugin Exports ────────────────────────────────────────── */

__declspec(dllexport) HWND __stdcall ListLoad(HWND pw, char* file, int flags) {
    ensure_ie11_emulation();
    load_settings();

    if(!g_classRegistered){
        WNDCLASSEXW wc={0}; wc.cbSize=sizeof(wc); wc.lpfnWndProc=ContainerWndProc;
        wc.hInstance=g_hInstance; wc.lpszClassName=CLASS_NAME;
        RegisterClassExW(&wc); g_classRegistered=1;
    }

    char* md=read_file_utf8(file); if(!md)return NULL;
    char* body=md_to_html(md); free(md); if(!body)return NULL;

    /* Determine theme: saved preference, or auto-detect */
    int dark = (g_settings.isDark >= 0) ? g_settings.isDark : is_dark_theme();

    /* Build CSS and JS dynamically with current settings */
    StrBuf cssBuf; sb_init(&cssBuf); build_css(&cssBuf);
    StrBuf jsBuf;  sb_init(&jsBuf);  build_js(&jsBuf);
    const char* ui = get_ui();

    size_t fl=strlen(body)+cssBuf.len+jsBuf.len+strlen(ui)+1024;
    char* full=(char*)malloc(fl);
    snprintf(full,fl,
        "<!DOCTYPE html><html%s><head>"
        "<meta http-equiv=\"X-UA-Compatible\" content=\"IE=edge\">"
        "<meta charset=\"utf-8\"><style>%s</style></head><body%s>"
        "%s<div id=\"mdv-ct\">%s</div>%s</body></html>",
        dark?" style=\"background:#1e1e1e\"":"", cssBuf.data, dark?" class=\"dark\"":"", ui, body, jsBuf.data);
    free(body); free(cssBuf.data); free(jsBuf.data);

    RECT rc; GetClientRect(pw,&rc);
    HWND hwnd=CreateWindowExW(0,CLASS_NAME,L"MDView",
        WS_CHILD|WS_VISIBLE|WS_CLIPCHILDREN,0,0,rc.right,rc.bottom,pw,NULL,g_hInstance,NULL);
    if(!hwnd){free(full);return NULL;}

    OleInitialize(NULL);
    MDViewData* data=(MDViewData*)calloc(1,sizeof(MDViewData));
    SetWindowLongPtrW(hwnd,GWLP_USERDATA,(LONG_PTR)data);

    SiteImpl* site=NULL;
    HRESULT hr=create_browser(hwnd,&data->pBrowser,&data->pOleObj,&site);
    if(FAILED(hr)){free(data);free(full);DestroyWindow(hwnd);return NULL;}

    IWebBrowser2_put_Left(data->pBrowser,0); IWebBrowser2_put_Top(data->pBrowser,0);
    IWebBrowser2_put_Width(data->pBrowser,rc.right); IWebBrowser2_put_Height(data->pBrowser,rc.bottom);
    IWebBrowser2_put_Silent(data->pBrowser, VARIANT_TRUE);
    navigate_to_html(data->pBrowser,full); free(full);

    IOleObject_DoVerb(data->pOleObj, OLEIVERB_UIACTIVATE, NULL,
                      (IOleClientSite*)&site->clientSite, 0, hwnd, &rc);

    /* Subclass the IE Server window for hotkeys */
    {
        HWND ieWnd = NULL;
        EnumChildWindows(hwnd, FindIEServerProc, (LPARAM)&ieWnd);
        if (ieWnd) {
            data->hwndIEServer = ieWnd;
            SetPropW(ieWnd, L"MDViewData", (HANDLE)data);
            data->origIEProc = (WNDPROC)SetWindowLongPtrW(ieWnd, GWLP_WNDPROC, (LONG_PTR)IEServerSubclassProc);
            SetFocus(ieWnd);
        }
    }

    return hwnd;
}

__declspec(dllexport) HWND __stdcall ListLoadW(HWND pw, WCHAR* file, int flags) {
    int len=WideCharToMultiByte(CP_UTF8,0,file,-1,NULL,0,NULL,NULL);
    char* u=(char*)malloc(len); WideCharToMultiByte(CP_UTF8,0,file,-1,u,len,NULL,NULL);
    HWND r=ListLoad(pw,u,flags); free(u); return r;
}

__declspec(dllexport) void __stdcall ListCloseWindow(HWND w) { DestroyWindow(w); }

__declspec(dllexport) void __stdcall ListGetDetectString(char* ds, int mx) {
    strncpy(ds,"EXT=\"MD\" | EXT=\"MARKDOWN\" | EXT=\"MKD\" | EXT=\"MKDN\"",mx-1); ds[mx-1]='\0';
}

__declspec(dllexport) int __stdcall ListSearchText(HWND w, int p, char* s) { return LISTPLUGIN_ERROR; }
__declspec(dllexport) int __stdcall ListSendCommand(HWND w, int c, int p) { return LISTPLUGIN_OK; }

__declspec(dllexport) void __stdcall ListSetDefaultParams(ListDefaultParamStruct* p) {
    if (p && p->DefaultIniName[0]) {
        strncpy(g_iniPath, p->DefaultIniName, MAX_PATH - 1);
        g_iniPath[MAX_PATH - 1] = '\0';
    }
}
