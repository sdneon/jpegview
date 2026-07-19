// Change size of image dialog
/////////////////////////////////////////////////////////////////////////////

#pragma once

#include "resource.h"
#include "ImageProcessingTypes.h"

// Allows to change image size, i.e. resample the image.
class CPwDlg : public CDialogImpl<CPwDlg>
{
public:
	enum { IDD = IDD_PASSWORD_DIALOG };

	BEGIN_MSG_MAP(CPwDlg)
		MESSAGE_HANDLER(WM_INITDIALOG, OnInitDialog)
		COMMAND_ID_HANDLER(IDOK, OnConfirmAndClose)
		COMMAND_ID_HANDLER(IDCANCEL, OnCancel)
	END_MSG_MAP()

	LRESULT OnInitDialog(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/);
	LRESULT OnConfirmAndClose(WORD /*wNotifyCode*/, WORD wID, HWND /*hWndCtl*/, BOOL& /*bHandled*/);
	LRESULT OnCancel(WORD /*wNotifyCode*/, WORD wID, HWND /*hWndCtl*/, BOOL& /*bHandled*/);

	CPwDlg(CString pw, CString title);
	~CPwDlg(void);

	// only valid when dialog confirmed with OK
	CString GetPw() { return m_pw; }

private:
	CButton m_btnOk;
	CButton m_btnCancel;
	CEdit m_edtPw;

	CString m_pw, m_title;
};
