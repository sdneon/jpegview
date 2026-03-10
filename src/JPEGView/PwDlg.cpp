#include "StdAfx.h"
#include "PwDlg.h"
#include "NLS.h"
#include "MaxImageDef.h"
#include "Helpers.h"

CPwDlg::CPwDlg(CString pw):
	m_pw(pw)
{
}

CPwDlg::~CPwDlg(void) {
}

LRESULT CPwDlg::OnInitDialog(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/) {
	this->SetWindowText(CNLS::GetString(_T("Enter Password")));

	m_btnOk.Attach(GetDlgItem(IDOK));
	m_btnCancel.Attach(GetDlgItem(IDCANCEL));

	m_edtPw.Attach(GetDlgItem(IDC_EDIT_PASSWORD));

	m_btnOk.SetWindowText(CNLS::GetString(_T("OK")));
	m_btnCancel.SetWindowText(CNLS::GetString(_T("Cancel")));

	m_edtPw.SetWindowText(m_pw);
	// Set the selection to the end of the text
	int textLength = m_edtPw.GetWindowTextLength();
	m_edtPw.SetSel(-1, textLength);

	return TRUE;
}

LRESULT CPwDlg::OnConfirmAndClose(WORD /*wNotifyCode*/, WORD wID, HWND /*hWndCtl*/, BOOL& /*bHandled*/) {
	CString pw;
	m_edtPw.GetWindowText(pw);
	m_pw = pw;
	EndDialog(IDOK);
	return 0;
}

LRESULT CPwDlg::OnCancel(WORD /*wNotifyCode*/, WORD wID, HWND /*hWndCtl*/, BOOL& /*bHandled*/) {
	EndDialog(IDCANCEL);
	return 0;
}
