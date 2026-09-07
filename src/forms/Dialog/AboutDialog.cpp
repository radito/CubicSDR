// Copyright (c) Charles J. Cliffe
// SPDX-License-Identifier: GPL-2.0+

#include "AboutDialog.h"

AboutDialog::AboutDialog( wxWindow* parent, wxWindowID id, const wxString& title, const wxPoint& pos, const wxSize& size, long style)
: AboutDialogBase(parent, id, title, pos, size, style) {
    m_appName->SetLabelText(CUBICSDR_INSTALL_NAME " v" CUBICSDR_VERSION);

    auto* forkPanel = new wxPanel(m_dbScroll, wxID_ANY);
    auto* forkSizer = new wxFlexGridSizer(0, 2, 2, 20);
    forkSizer->SetFlexibleDirection(wxBOTH);
    forkSizer->SetNonFlexibleGrowMode(wxFLEX_GROWMODE_ALL);

    auto* forkHeader = new wxStaticText(forkPanel, wxID_ANY, wxT("Forked By"));
    forkHeader->SetFont(wxFont(15, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
                               wxFONTWEIGHT_NORMAL, false, wxEmptyString));
    forkSizer->Add(forkHeader, 0, wxALL, 5);

    auto* githubHeader = new wxStaticText(forkPanel, wxID_ANY, wxT("GitHub"));
    githubHeader->SetFont(wxFont(wxNORMAL_FONT->GetPointSize(),
                                 wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
                                 wxFONTWEIGHT_NORMAL, false, wxEmptyString));
    forkSizer->Add(githubHeader, 0, wxALL, 5);

    forkSizer->Add(new wxStaticText(forkPanel, wxID_ANY, wxT("Radito")),
                   0, wxALL, 5);
    forkSizer->Add(new wxStaticText(forkPanel, wxID_ANY, wxT("@radito")),
                   0, wxALL, 5);

    forkPanel->SetSizer(forkSizer);
    forkPanel->Layout();
    forkSizer->Fit(forkPanel);

    // Place fork attribution between the original developers and contributors.
    m_dbScroll->GetSizer()->Insert(
        1, forkPanel, 0, wxALL | wxEXPAND | wxRESERVE_SPACE_EVEN_IF_HIDDEN, 5);
    m_dbScroll->Layout();
    m_dbScroll->FitInside();
}
