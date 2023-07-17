// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "precomp.h"
#include "CopyToCharPopup.hpp"

#include "stream.h"
#include "_stream.h"
#include "resource.h"

static constexpr size_t COPY_TO_CHAR_PROMPT_LENGTH = 26;

CopyToCharPopup::CopyToCharPopup(SCREEN_INFORMATION& screenInfo) :
    Popup(screenInfo, { COPY_TO_CHAR_PROMPT_LENGTH + 2, 1 })
{
}

// Routine Description:
// - This routine handles the delete char popup.  It returns when we're out of input or the user has entered a char.
// Return Value:
// - CONSOLE_STATUS_WAIT - we ran out of input, so a wait block was created
// - CONSOLE_STATUS_READ_COMPLETE - user hit return
[[nodiscard]] NTSTATUS CopyToCharPopup::Process(COOKED_READ_DATA& cookedReadData) noexcept
{
    auto wch = UNICODE_NULL;
    auto popupKey = false;
    DWORD modifiers = 0;
    auto Status = _getUserInput(cookedReadData, popupKey, modifiers, wch);
    if (FAILED_NTSTATUS(Status))
    {
        return Status;
    }

    CommandLine::Instance().EndCurrentPopup();

    if (popupKey && wch == VK_ESCAPE)
    {
        return CONSOLE_STATUS_WAIT_NO_BLOCK;
    }

    return CONSOLE_STATUS_WAIT_NO_BLOCK;
}

void CopyToCharPopup::_DrawContent()
{
    _DrawPrompt(ID_CONSOLE_MSGCMDLINEF2);
}
