// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "precomp.h"

#include "cmdline.h"
#include "popup.h"
#include "CommandNumberPopup.hpp"
#include "CommandListPopup.hpp"
#include "CopyFromCharPopup.hpp"
#include "CopyToCharPopup.hpp"

#include "_output.h"
#include "output.h"
#include "stream.h"
#include "_stream.h"
#include "dbcs.h"
#include "handle.h"
#include "misc.h"
#include "../types/inc/convert.hpp"
#include "srvinit.h"

#include "ApiRoutines.h"

#include "../interactivity/inc/ServiceLocator.hpp"

#pragma warning(disable : 4100)

#pragma hdrstop
using Microsoft::Console::Interactivity::ServiceLocator;

// Routine Description:
// - This routine validates a string buffer and returns the pointers of where the strings start within the buffer.
// Arguments:
// - Unicode - Supplies a boolean that is TRUE if the buffer contains Unicode strings, FALSE otherwise.
// - Buffer - Supplies the buffer to be validated.
// - Size - Supplies the size, in bytes, of the buffer to be validated.
// - Count - Supplies the expected number of strings in the buffer.
// ... - Supplies a pair of arguments per expected string. The first one is the expected size, in bytes, of the string
//       and the second one receives a pointer to where the string starts.
// Return Value:
// - TRUE if the buffer is valid, FALSE otherwise.
bool IsValidStringBuffer(_In_ bool Unicode, _In_reads_bytes_(Size) PVOID Buffer, _In_ ULONG Size, _In_ ULONG Count, ...)
{
    va_list Marker;
    va_start(Marker, Count);

    while (Count > 0)
    {
        const auto StringSize = va_arg(Marker, ULONG);
        const auto StringStart = va_arg(Marker, PVOID*);

        // Make sure the string fits in the supplied buffer and that it is properly aligned.
        if (StringSize > Size)
        {
            break;
        }

        if (Unicode && (StringSize % sizeof(WCHAR)) != 0)
        {
            break;
        }

        *StringStart = Buffer;

        // Go to the next string.
        Buffer = RtlOffsetToPointer(Buffer, StringSize);
        Size -= StringSize;
        Count -= 1;
    }

    va_end(Marker);

    return Count == 0;
}

// Routine Description:
// - Detects Word delimiters
bool IsWordDelim(const wchar_t wch)
{
    // the space character is always a word delimiter. Do not add it to the WordDelimiters global because
    // that contains the user configurable word delimiters only.
    if (wch == UNICODE_SPACE)
    {
        return true;
    }
    const auto& delimiters = ServiceLocator::LocateGlobals().WordDelimiters;
    return std::ranges::find(delimiters, wch) != delimiters.end();
}

bool IsWordDelim(const std::wstring_view charData)
{
    return charData.size() == 1 && IsWordDelim(charData.front());
}

CommandLine::CommandLine() :
    _isVisible{ true }
{
}

CommandLine& CommandLine::Instance()
{
    static CommandLine c;
    return c;
}

bool CommandLine::IsEditLineEmpty()
{
    const auto& gci = ServiceLocator::LocateGlobals().getConsoleInformation();

    if (!gci.HasPendingCookedRead())
    {
        // If the cooked read data pointer is null, there is no edit line data and therefore it's empty.
        return true;
    }
    else
    {
        return false;
    }
}

void CommandLine::Hide(const bool fUpdateFields)
{
}

void CommandLine::Show()
{
}

// Routine Description:
// - Returns true if the commandline is currently being displayed. This is false
//      after Hide() is called, and before Show() is called again.
// Return Value:
// - true if the commandline should be displayed. Does not take into account
//   the echo state of the input. This is only controlled by calls to Hide/Show
bool CommandLine::IsVisible() const noexcept
{
    return _isVisible;
}

// Routine Description:
// - stops the current popup
void CommandLine::EndCurrentPopup()
{
    if (!_popups.empty())
    {
        _popups.front()->End();
        _popups.pop_front();
    }
}

// Routine Description:
// - stops all popups
void CommandLine::EndAllPopups()
{
    while (!_popups.empty())
    {
        _popups.front()->End();
        _popups.pop_front();
    }
}

void CommandLine::_deletePromptAfterCursor(COOKED_READ_DATA& cookedReadData) noexcept
{
}

// Routine Description:
// - Deletes a glyph from the right side of the cursor
// Arguments:
// - cookedReadData - The cooked read data to operate on
// Return Value:
// - The new cursor position
til::point CommandLine::_deleteFromRightOfCursor(COOKED_READ_DATA& cookedReadData) noexcept
{
    return {};
}

// Routine Description:
// - This routine process command line editing keys.
// Return Value:
// - CONSOLE_STATUS_WAIT - CommandListPopup ran out of input
// - CONSOLE_STATUS_READ_COMPLETE - user hit <enter> in CommandListPopup
// - STATUS_SUCCESS - everything's cool
[[nodiscard]] NTSTATUS CommandLine::ProcessCommandLine(COOKED_READ_DATA& cookedReadData, _In_ WCHAR wch, const DWORD dwKeyState)
try
{
    auto cursorPosition = til::point{};
    NTSTATUS Status = STATUS_SUCCESS;

    const auto altPressed = WI_IsAnyFlagSet(dwKeyState, LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED);
    const auto ctrlPressed = WI_IsAnyFlagSet(dwKeyState, LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED);
    auto UpdateCursorPosition = false;
    switch (wch)
    {
    case VK_ESCAPE:
        break;
    case VK_DOWN:
        //_processHistoryCycling(cookedReadData, CommandHistory::SearchDirection::Next);
        break;
    case VK_UP:
    case VK_F5:
        //_processHistoryCycling(cookedReadData, CommandHistory::SearchDirection::Previous);
        break;
    case VK_PRIOR:
        //_setPromptToOldestCommand(cookedReadData);
        break;
    case VK_NEXT:
        //_setPromptToNewestCommand(cookedReadData);
        break;
    case VK_END:
        if (ctrlPressed)
        {
            _deletePromptAfterCursor(cookedReadData);
        }
        else
        {
            //cursorPosition = _moveCursorToEndOfPrompt(cookedReadData);
            UpdateCursorPosition = true;
        }
        break;
    case VK_HOME:
        if (ctrlPressed)
        {
            //cursorPosition = _deletePromptBeforeCursor(cookedReadData);
            UpdateCursorPosition = true;
        }
        else
        {
            //cursorPosition = _moveCursorToStartOfPrompt(cookedReadData);
            UpdateCursorPosition = true;
        }
        break;
    case VK_LEFT:
        if (ctrlPressed)
        {
            //cursorPosition = _moveCursorLeftByWord(cookedReadData);
            UpdateCursorPosition = true;
        }
        else
        {
            //cursorPosition = _moveCursorLeft(cookedReadData);
            UpdateCursorPosition = true;
        }
        break;
    case VK_F1:
    {
        // we don't need to check for end of buffer here because we've
        // already done it.
        //cursorPosition = _moveCursorRight(cookedReadData);
        UpdateCursorPosition = true;
        break;
    }
    case VK_RIGHT:
        // we don't need to check for end of buffer here because we've
        // already done it.
        if (ctrlPressed)
        {
            //cursorPosition = _moveCursorRightByWord(cookedReadData);
            UpdateCursorPosition = true;
        }
        else
        {
            //cursorPosition = _moveCursorRight(cookedReadData);
            UpdateCursorPosition = true;
        }
        break;
    case VK_F2:
    {
        //Status = _startCopyToCharPopup(cookedReadData);
        if (S_FALSE == Status)
        {
            // We couldn't make the popup, so loop around and read the next character.
            break;
        }
        else
        {
            return Status;
        }
    }
    case VK_F3:
        //_fillPromptWithPreviousCommandFragment(cookedReadData);
        break;
    case VK_F4:
    {
        //Status = _startCopyFromCharPopup(cookedReadData);
        if (S_FALSE == Status)
        {
            // We couldn't display a popup. Go around a loop behind.
            break;
        }
        else
        {
            return Status;
        }
    }
    case VK_F6:
    {
        //_insertCtrlZ(cookedReadData);
        break;
    }
    case VK_F7:
        if (!ctrlPressed && !altPressed)
        {
            //Status = _startCommandListPopup(cookedReadData);
        }
        else if (altPressed)
        {
            //_deleteCommandHistory(cookedReadData);
        }
        break;

    case VK_F8:
        //cursorPosition = _cycleMatchingCommandHistoryToPrompt(cookedReadData);
        UpdateCursorPosition = true;
    case VK_F9:
    {
        Status = StartCommandNumberPopup(cookedReadData);
        if (S_FALSE == Status)
        {
            // If we couldn't make the popup, break and go around to read another input character.
            break;
        }
        else
        {
            return Status;
        }
    }
    case VK_F10:
        // Alt+F10 clears the aliases for specifically cmd.exe.
        if (altPressed)
        {
            Alias::s_ClearCmdExeAliases();
        }
        break;
    case VK_INSERT:
        break;
    case VK_DELETE:
        cursorPosition = _deleteFromRightOfCursor(cookedReadData);
        UpdateCursorPosition = true;
        break;
    default:
        return E_NOTIMPL;
    }

    return Status;
}
NT_CATCH_RETURN()

HRESULT CommandLine::StartCommandNumberPopup(COOKED_READ_DATA& cookedReadData)
{
    return 0;
}
