// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "precomp.h"
#include "CommandNumberPopup.hpp"

#include "stream.h"
#include "_stream.h"
#include "cmdline.h"
#include "resource.h"

#include "../interactivity/inc/ServiceLocator.hpp"

// 5 digit number for command history
static constexpr size_t COMMAND_NUMBER_LENGTH = 5;

static constexpr size_t COMMAND_NUMBER_PROMPT_LENGTH = 22;

CommandNumberPopup::CommandNumberPopup(SCREEN_INFORMATION& screenInfo) :
    Popup(screenInfo, { COMMAND_NUMBER_PROMPT_LENGTH + COMMAND_NUMBER_LENGTH, 1 })
{
    _userInput.reserve(COMMAND_NUMBER_LENGTH);
}

// Routine Description:
// - This routine handles the command number selection popup.
// Return Value:
// - CONSOLE_STATUS_WAIT - we ran out of input, so a wait block was created
// - CONSOLE_STATUS_READ_COMPLETE - user hit return
[[nodiscard]] NTSTATUS CommandNumberPopup::Process(COOKED_READ_DATA& cookedReadData) noexcept
{
    auto Status = STATUS_SUCCESS;
    auto wch = UNICODE_NULL;
    auto popupKeys = false;
    DWORD modifiers = 0;

    for (;;)
    {
        Status = _getUserInput(cookedReadData, popupKeys, modifiers, wch);
        if (FAILED_NTSTATUS(Status))
        {
            return Status;
        }

        if (std::iswdigit(wch))
        {
        }
        else if (wch == UNICODE_BACKSPACE)
        {
        }
        else if (wch == VK_ESCAPE)
        {
            //CommandLine::Instance().EndAllPopups();
            //LOG_IF_FAILED(cookedReadData.ScreenInfo().SetCursorPosition(cookedReadData.BeforeDialogCursorPosition(), TRUE));
            break;
        }
        else if (wch == UNICODE_CARRIAGERETURN)
        {
            //const auto commandNumber = gsl::narrow<short>(std::min(static_cast<size_t>(_parse()), cookedReadData.History().GetNumberOfCommands() - 1));
            //CommandLine::Instance().EndAllPopups();
            //SetCurrentCommandLine(cookedReadData, commandNumber);
            break;
        }
    }
    return CONSOLE_STATUS_WAIT_NO_BLOCK;
}

void CommandNumberPopup::_DrawContent()
{
    _DrawPrompt(ID_CONSOLE_MSGCMDLINEF9);
}
