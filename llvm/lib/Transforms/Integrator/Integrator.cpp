// This pass performs function inlining, loop peeling, load forwarding and dead instruction elimination
// in concert. All analysis is performed by HypotheticalConstantFolder; this pass is solely responsbible for
// taking user input regarding what will be integrated (perhaps showing a GUI for this purpose) and actually
// committing the results to the module under consideration.

#define DEBUG_TYPE "integrator"

#include "llvm/Pass.h"
#include "llvm/Module.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Analysis/HypotheticalConstantFolder.h"
#include "llvm/Support/raw_ostream.h"

#include <wx/wx.h>
#include <wx/splitter.h>
#include <wx/sizer.h>
#include <wx/dataview.h>
#include <wx/bitmap.h>

#include <errno.h>
#include <string.h>

using namespace llvm;

// For communication with wxWidgets, since there doesn't seem to be any easy way
// of passing a parameter to WxApp's constructor.
static IntegrationHeuristicsPass* IHP;

namespace {

  class Integrator : public ModulePass {
  public:

    static char ID;
    Integrator() : ModulePass(ID) {}

    bool runOnModule(Module& M);

    virtual void getAnalysisUsage(AnalysisUsage &AU) const;

  };

}

char Integrator::ID = 0;
INITIALIZE_PASS(Integrator, "integrator", "Pervasive integration", false, false);

Pass* llvm::createIntegratorPass() { return new Integrator(); }

// Implement a GUI for leafing through integration results

class IntegratorApp : public wxApp {

  virtual bool OnInit();

};

static char workdir[] = "/tmp/integrator_XXXXXX";

class IntegratorFrame: public wxFrame
{

  wxBitmap* currentBitmap;
  wxStaticBitmap* image;

  std::string dotpath;
  std::string pngpath;
  std::string dotcommand;

public:

  IntegratorFrame(const wxString& title, const wxPoint& pos, const wxSize& size);

  void OnClose(wxCloseEvent&);
  void OnQuit(wxCommandEvent& event);
  void OnSelectionChanged(wxDataViewEvent&);

  DECLARE_EVENT_TABLE()

};

enum
{
  ID_Quit = wxID_HIGHEST + 1,
  ID_TreeView,
  ID_SelectionChanged
};

BEGIN_EVENT_TABLE(IntegratorFrame, wxFrame)
  EVT_CLOSE(IntegratorFrame::OnClose)
  EVT_MENU(ID_Quit, IntegratorFrame::OnQuit)
  EVT_DATAVIEW_SELECTION_CHANGED(ID_TreeView, IntegratorFrame::OnSelectionChanged)
END_EVENT_TABLE()

bool IntegratorApp::OnInit() {

  wxImage::AddHandler(new wxPNGHandler);
  IntegratorFrame *frame = new IntegratorFrame( _("Integrator"), wxPoint(50, 50), wxSize(1000, 600));
  frame->Show(true);
  SetTopWindow(frame);
  return true;

}

class IntHeuristicsModel: public wxDataViewModel {

  IntegrationAttempt* Root;

public:
  IntHeuristicsModel(IntegrationAttempt* _Root): wxDataViewModel(), Root(_Root) { }
  ~IntHeuristicsModel() {}
  unsigned int GetColumnCount() const {
    return 1;
  }
  wxString GetColumnType(unsigned int column) const {
    return "string";
  }
  void GetValue(wxVariant& val, const wxDataViewItem& item, unsigned int column) const {

    assert(column == 0);
    assert(item.IsOk());

    struct IntegratorTag* tag = (struct IntegratorTag*)item.GetID();
    if(!tag) {
      val = wxEmptyString;
      return;
    }
    
    switch(tag->type) {
    case IntegratorTypeIA:
      {
	IntegrationAttempt* IA = (IntegrationAttempt*)tag->ptr;
	val = _(IA->getShortHeader());
      }
      break;
    case IntegratorTypePA:
      {
	PeelAttempt* PA = (PeelAttempt*)tag->ptr;
	val = _(PA->getShortHeader());
      }
      break;
    default:
      assert(0 && "Invalid node tag");
    }

  }

  bool SetValue(const wxVariant& val, const wxDataViewItem& item, unsigned int column) {
    return true;
  }

  wxDataViewItem GetParent(const wxDataViewItem& item) const {

    struct IntegratorTag* tag = (struct IntegratorTag*)item.GetID();
    if(!tag)
      return wxDataViewItem(0);

    switch(tag->type) {
    case IntegratorTypeIA:
      {
	IntegrationAttempt* IA = (IntegrationAttempt*)tag->ptr;
	IntegratorTag* parentTag = IA->getParentTag();
	if(!parentTag)
	  return wxDataViewItem(0);
	else
	  return wxDataViewItem((void*)parentTag);
      }
      break;
    case IntegratorTypePA:
      {
	PeelAttempt* PA = (PeelAttempt*)tag->ptr;
	return wxDataViewItem((void*)PA->getParentTag());
      }
      break;
    default:
      assert(0 && "Invalid node tag");
    }

  }

  bool IsContainer(const wxDataViewItem& item) const {

    struct IntegratorTag* tag = (struct IntegratorTag*)item.GetID();

    if(!tag)
      return true; // Root node has children

    switch(tag->type) {
    case IntegratorTypeIA:
      {
	IntegrationAttempt* IA = (IntegrationAttempt*)tag->ptr;
	return IA->hasChildren();
      }
      break;
    case IntegratorTypePA:
      {
	return true;
      }
      break;
    default:
      assert(0 && "Invalid node tag");
    }

  }

  unsigned GetChildren(const wxDataViewItem& item, wxDataViewItemArray& children) const {

    struct IntegratorTag* tag = (struct IntegratorTag*)item.GetID();
    if(!tag) {

      // Root node:
      children.Add(wxDataViewItem((void*)&(Root->tag)));
      return 1;

    }

    switch(tag->type) {
    case IntegratorTypeIA:
      {
	IntegrationAttempt* IA = (IntegrationAttempt*)tag->ptr;
	for(unsigned i = 0; i < IA->getNumChildren(); ++i) {
	  children.Add(wxDataViewItem((void*)IA->getChildTag(i)));
	}
	return IA->getNumChildren();
      }
      break;
    case IntegratorTypePA:
      {
	PeelAttempt* PA = (PeelAttempt*)tag->ptr;
	for(unsigned i = 0; i < PA->getNumChildren(); ++i) {
	  children.Add(wxDataViewItem((void*)PA->getChildTag(i)));
	}
	return PA->getNumChildren();
      }
      break;
    default:
      assert(0 && "Invalid node tag");
    }

  }

};

IntegratorFrame::IntegratorFrame(const wxString& title, const wxPoint& pos, const wxSize& size)
  : wxFrame(NULL, -1, title, pos, size) {

  if(!mkdtemp(workdir)) {
    errs() << "Failed to create a temporary directory: " << strerror(errno) << "\n";
    exit(1);
  }

  {
    raw_string_ostream ROS(dotpath);
    ROS << workdir << "/out.dot";
    ROS.flush();
  }
  {
    raw_string_ostream ROS(pngpath);
    ROS << workdir << "/out.png";
    ROS.flush();
  }
  {
    raw_string_ostream ROS(dotcommand);
    ROS << "dot " << dotpath << " -o " << pngpath << " -Tpng";
    ROS.flush();
  }

  wxMenu *menuFile = new wxMenu;

  menuFile->Append( ID_Quit, _("E&xit") );

  wxMenuBar *menuBar = new wxMenuBar;
  menuBar->Append( menuFile, _("&File") );

  SetMenuBar( menuBar );

  wxBoxSizer* sizerMain = new wxBoxSizer(wxVERTICAL);
  wxSplitterWindow* splitter = new wxSplitterWindow(this, wxID_ANY);
  splitter->SetSashGravity(0);
  splitter->SetMinimumPaneSize(20);
  sizerMain->Add(splitter, 1, wxEXPAND, 0);

  wxPanel* menuPanel = new wxPanel(splitter, wxID_ANY);
  
  wxBoxSizer* menuPanelSizer = new wxBoxSizer(wxVERTICAL);

  wxDataViewCtrl* menuPanelData = new wxDataViewCtrl(menuPanel, ID_TreeView);
  wxDataViewTextRenderer* col0rend = new wxDataViewTextRenderer("string", wxDATAVIEW_CELL_INERT);
  wxDataViewColumn* col0 = new wxDataViewColumn("Name", col0rend, 0, 200, wxALIGN_LEFT, wxDATAVIEW_COL_RESIZABLE);
  menuPanelData->AppendColumn(col0);

  IntHeuristicsModel* model = new IntHeuristicsModel(IHP->getRoot());
  menuPanelData->AssociateModel(model);

  menuPanelSizer->Add(menuPanelData, 1, wxEXPAND, 0);

  menuPanel->SetSizer(menuPanelSizer);

  wxScrolledWindow* imagePanel = new wxScrolledWindow(splitter, wxID_ANY);
  imagePanel->SetScrollRate(1, 1);
  
  wxBoxSizer* imagePanelSizer = new wxBoxSizer(wxVERTICAL);

  currentBitmap = new wxBitmap(1, 1);

  image = new wxStaticBitmap(imagePanel, wxID_ANY, *currentBitmap);
  imagePanelSizer->Add(image, 1, wxEXPAND, 0);
  imagePanel->SetSizer(imagePanelSizer);
  
  splitter->SplitVertically(menuPanel, imagePanel);

  this->SetSizer(sizerMain);

}

void IntegratorFrame::OnQuit(wxCommandEvent& WXUNUSED(event)) {
  Close(true);
}

void IntegratorFrame::OnClose(wxCloseEvent& WXUNUSED(event)) {

  std::string command;
  raw_string_ostream ROS(command);
  ROS << "rm -rf " << workdir;
  ROS.flush();

  if(system(command.c_str()) != 0) {

    errs() << "Warning: failed to delete " << workdir << "\n";

  }

  Destroy();

}

void IntegratorFrame::OnSelectionChanged(wxDataViewEvent& event) {

  wxDataViewItem item = event.GetItem();
  
  IntegratorTag* tag = (IntegratorTag*)item.GetID();
  if(tag && tag->type == IntegratorTypeIA) {

    delete currentBitmap;
    currentBitmap = 0;

    std::string error;
    raw_fd_ostream RFO(dotpath.c_str(), error);
    ((IntegrationAttempt*)tag->ptr)->describeAsDOT(RFO);
    RFO.close();

    if(!error.empty()) {

      errs() << "Failed to open " << dotpath << ": " << error << "\n";

    }
    else {

      if(int ret = system(dotcommand.c_str()) != 0) {

	errs() << "Failed to run '" << dotcommand << "' (returned " << ret << ")\n";
	
      }
      else {

	currentBitmap = new wxBitmap(_(pngpath), wxBITMAP_TYPE_PNG);

      }

    }

    if(!currentBitmap)
      currentBitmap = new wxBitmap(1, 1);

    image->SetBitmap(*currentBitmap);
   
  }

}

IMPLEMENT_APP_NO_MAIN(IntegratorApp)

bool Integrator::runOnModule(Module& M) {

  IHP = &getAnalysis<IntegrationHeuristicsPass>();

  int argc = 0;
  char** argv = 0;
  wxEntry(argc, argv);

  return false;

}

void Integrator::getAnalysisUsage(AnalysisUsage& AU) const {

  AU.addRequired<IntegrationHeuristicsPass>();

}


