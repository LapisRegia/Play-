#ifndef _CALLSTACKWND_H_
#define _CALLSTACKWND_H_

#include <boost/signal.hpp>
#include "win32/MDIChild.h"
#include "win32/ListView.h"
#include "Types.h"
#include "../VirtualMachine.h"

class CMIPS;

class CCallStackWnd : public Framework::Win32::CMDIChild, public boost::signals::trackable
{
public:
									CCallStackWnd(HWND, CVirtualMachine&, CMIPS*);
	virtual							~CCallStackWnd();
	boost::signal<void (uint32)>	m_OnFunctionDblClick;

protected:
	long							OnSize(unsigned int, unsigned int, unsigned int);
	long							OnSysCommand(unsigned int, LPARAM);
	long							OnNotify(WPARAM, NMHDR*);

private:
	void							RefreshLayout();
	void							CreateColumns();
	void							Update();
	void							OnListDblClick();

	void							OnMachineStateChange();
	void							OnRunningStateChange();

    CVirtualMachine&                m_virtualMachine;
    CMIPS*							m_pCtx;
	Framework::Win32::CListView*	m_pList;
};

#endif
