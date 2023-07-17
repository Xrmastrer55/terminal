// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "precomp.h"
#include "readDataCooked.hpp"
#include "dbcs.h"
#include "stream.h"
#include "misc.h"
#include "_stream.h"
#include "inputBuffer.hpp"
#include "cmdline.h"
#include "../types/inc/GlyphWidth.hpp"
#include "../types/inc/convert.hpp"

#pragma warning(disable : 4100 4189)

#include "../interactivity/inc/ServiceLocator.hpp"
#include "til/unicode.h"

#define LINE_INPUT_BUFFER_SIZE (256 * sizeof(WCHAR))

using Microsoft::Console::Interactivity::ServiceLocator;

// Routine Description:
// - Constructs cooked read data class to hold context across key presses while a user is modifying their 'input line'.
// Arguments:
// - pInputBuffer - Buffer that data will be read from.
// - pInputReadHandleData - Context stored across calls from the same input handle to return partial data appropriately.
// - screenInfo - Output buffer that will be used for 'echoing' the line back to the user so they can see/manipulate it
// - BufferSize -
// - BytesRead -
// - CurrentPosition -
// - BufPtr -
// - BackupLimit -
// - UserBufferSize - The byte count of the buffer presented by the client
// - UserBuffer - The buffer that was presented by the client for filling with input data on read conclusion/return from server/host.
// - OriginalCursorPosition -
// - NumberOfVisibleChars
// - CtrlWakeupMask - Special client parameter to interrupt editing, end the wait, and return control to the client application
// - Echo -
// - InsertMode -
// - Processed -
// - Line -
// - pTempHandle - A handle to the output buffer to prevent it from being destroyed while we're using it to present 'edit line' text.
// - initialData - any text data that should be prepopulated into the buffer
// - pClientProcess - Attached process handle object
// Return Value:
// - THROW: Throws E_INVALIDARG for invalid pointers.
COOKED_READ_DATA::COOKED_READ_DATA(_In_ InputBuffer* const pInputBuffer,
                                   _In_ INPUT_READ_HANDLE_DATA* const pInputReadHandleData,
                                   SCREEN_INFORMATION& screenInfo,
                                   _In_ size_t UserBufferSize,
                                   _In_ char* UserBuffer,
                                   _In_ ULONG CtrlWakeupMask,
                                   _In_ const std::wstring_view exeName,
                                   _In_ const std::wstring_view initialData,
                                   _In_ ConsoleProcessHandle* const pClientProcess) :
    ReadData(pInputBuffer, pInputReadHandleData),
    _screenInfo{ screenInfo },
    _userBuffer{ UserBuffer, UserBufferSize },
    _buffer{ initialData },
    _ctrlWakeupMask{ CtrlWakeupMask }
{
    _positionBeg = screenInfo.GetTextBuffer().GetCursor().GetPosition();
}

COOKED_READ_DATA::~COOKED_READ_DATA()
{
    CommandLine::Instance().EndAllPopups();
}

// Routine Description:
// - This routine is called to complete a cooked read that blocked in ReadInputBuffer.
// - The context of the read was saved in the CookedReadData structure.
// - This routine is called when events have been written to the input buffer.
// - It is called in the context of the writing thread.
// - It may be called more than once.
// Arguments:
// - TerminationReason - if this routine is called because a ctrl-c or ctrl-break was seen, this argument
//                      contains CtrlC or CtrlBreak. If the owning thread is exiting, it will have ThreadDying. Otherwise 0.
// - fIsUnicode - Whether to convert the final data to A (using Console Input CP) at the end or treat everything as Unicode (UCS-2)
// - pReplyStatus - The status code to return to the client application that originally called the API (before it was queued to wait)
// - pNumBytes - The number of bytes of data that the server/driver will need to transmit back to the client process
// - pControlKeyState - For certain types of reads, this specifies which modifier keys were held.
// - pOutputData - not used
// Return Value:
// - true if the wait is done and result buffer/status code can be sent back to the client.
// - false if we need to continue to wait until more data is available.
bool COOKED_READ_DATA::Notify(const WaitTerminationReason TerminationReason,
                              const bool fIsUnicode,
                              _Out_ NTSTATUS* const pReplyStatus,
                              _Out_ size_t* const pNumBytes,
                              _Out_ DWORD* const pControlKeyState,
                              _Out_ void* const /*pOutputData*/) noexcept
try
{
    auto& gci = ServiceLocator::LocateGlobals().getConsoleInformation();

    *pNumBytes = 0;
    *pControlKeyState = 0;
    *pReplyStatus = STATUS_SUCCESS;

    // if ctrl-c or ctrl-break was seen, terminate read.
    if (WI_IsAnyFlagSet(TerminationReason, (WaitTerminationReason::CtrlC | WaitTerminationReason::CtrlBreak)))
    {
        *pReplyStatus = STATUS_ALERTED;
        gci.SetCookedReadData(nullptr);
        return true;
    }

    // See if we were called because the thread that owns this wait block is exiting.
    if (WI_IsFlagSet(TerminationReason, WaitTerminationReason::ThreadDying))
    {
        *pReplyStatus = STATUS_THREAD_IS_TERMINATING;
        gci.SetCookedReadData(nullptr);
        return true;
    }

    // We must see if we were woken up because the handle is being closed. If
    // so, we decrement the read count. If it goes to zero, we wake up the
    // close thread. Otherwise, we wake up any other thread waiting for data.
    if (WI_IsFlagSet(TerminationReason, WaitTerminationReason::HandleClosing))
    {
        *pReplyStatus = STATUS_ALERTED;
        gci.SetCookedReadData(nullptr);
        return true;
    }

    if (Read(fIsUnicode, *pNumBytes, *pControlKeyState))
    {
        gci.SetCookedReadData(nullptr);
        return true;
    }

    return false;
}
NT_CATCH_RETURN()

void COOKED_READ_DATA::MigrateUserBuffersOnTransitionToBackgroundWait(const void* oldBuffer, void* newBuffer) noexcept
{
    // See the comment in WaitBlock.cpp for more information.
    if (_userBuffer.data() == oldBuffer)
    {
        _userBuffer = { static_cast<char*>(newBuffer), _userBuffer.size() };
    }
}

// Routine Description:
// - Method that actually retrieves a character/input record from the buffer (key press form)
//   and determines the next action based on the various possible cooked read modes.
// - Mode options include the F-keys popup menus, keyboard manipulation of the edit line, etc.
// - This method also does the actual copying of the final manipulated data into the return buffer.
// Arguments:
// - isUnicode - Treat as UCS-2 unicode or use Input CP to convert when done.
// - numBytes - On in, the number of bytes available in the client
// buffer. On out, the number of bytes consumed in the client buffer.
// - controlKeyState - For some types of reads, this is the modifier key state with the last button press.
bool COOKED_READ_DATA::Read(const bool isUnicode, size_t& numBytes, ULONG& controlKeyState)
{
    controlKeyState = 0;

    if (!_readCharInputLoop(isUnicode, numBytes))
    {
        return false;
    }

    _handlePostCharInputLoop(isUnicode, numBytes, controlKeyState);
    return true;
}

void COOKED_READ_DATA::EraseBeforeResize()
{
    if (_length <= 0)
    {
        return;
    }

    LOG_IF_FAILED(_screenInfo.SetCursorPosition(_positionBeg, false));

    do
    {
        const auto erase = std::min(8, _length);
        WriteCharsLegacy(_screenInfo, 0, { L"        ", gsl::narrow_cast<size_t>(erase) });
        _length -= erase;
    } while (_length > 0);
}

void COOKED_READ_DATA::RedrawAfterResize()
{
    _positionBeg = _screenInfo.GetTextBuffer().GetCursor().GetPosition();
}

// Routine Description:
// - saves data in the prompt buffer as pending input
// Arguments:
// - isUnicode - Treat as UCS-2 unicode or use Input CP to convert when done.
// - numBytes - On in, the number of bytes available in the client
// buffer. On out, the number of bytes consumed in the client buffer.
// Return Value:
// - Status code that indicates success, wait, etc.
bool COOKED_READ_DATA::_readCharInputLoop(const bool isUnicode, size_t& numBytes)
{
    for (;;)
    {
        auto wch = UNICODE_NULL;
        auto commandLineEditingKeys = false;
        DWORD keyState = 0;

        // This call to GetChar may block.
        auto Status = GetChar(_pInputBuffer, &wch, true, &commandLineEditingKeys, nullptr, &keyState);
        if (Status == CONSOLE_STATUS_WAIT)
        {
            return false;
        }
        THROW_IF_NTSTATUS_FAILED(Status);

        if (commandLineEditingKeys)
        {
            //Status = CommandLine::Instance().ProcessCommandLine(*this, wch, keyState);
            //if (Status == CONSOLE_STATUS_READ_COMPLETE || Status == CONSOLE_STATUS_WAIT)
            //{
            //    break;
            //}
            //if (FAILED_NTSTATUS(Status))
            //{
            //    if (Status == CONSOLE_STATUS_WAIT_NO_BLOCK)
            //    {
            //        Status = CONSOLE_STATUS_WAIT;
            //    }
            //    else
            //    {
            //        _buffer.clear();
            //    }
            //    break;
            //}
        }
        else
        {
            if (_processInput(wch, keyState))
            {
                auto& gci = ServiceLocator::LocateGlobals().getConsoleInformation();
                gci.Flags |= CONSOLE_IGNORE_NEXT_KEYUP;
                return true;
            }
        }
    }
}

// Routine Description:
// - handles any tasks that need to be completed after the read input loop finishes
// Arguments:
// - isUnicode - Treat as UCS-2 unicode or use Input CP to convert when done.
// - numBytes - On in, the number of bytes available in the client
// buffer. On out, the number of bytes consumed in the client buffer.
// - controlKeyState - For some types of reads, this is the modifier key state with the last button press.
// Return Value:
// - Status code that indicates success, out of memory, etc.
void COOKED_READ_DATA::_handlePostCharInputLoop(const bool isUnicode, size_t& numBytes, ULONG& controlKeyState)
{
    auto writer = _userBuffer;
    std::wstring_view input{ _buffer };
    DWORD LineCount = 1;

    //if (_echoInput)
    {
        const auto idx = input.find(UNICODE_CARRIAGERETURN);
        if (idx != decltype(input)::npos)
        {
            //if (_commandHistory)
            //{
            //    auto& gci = ServiceLocator::LocateGlobals().getConsoleInformation();
            //    LOG_IF_FAILED(_commandHistory->Add({ _backupLimit, idx }, WI_IsFlagSet(gci.Flags, CONSOLE_HISTORY_NODUP)));
            //}

            // Don't be fooled by ProcessAliases only taking one argument. It rewrites multiple
            // class members on return, including `_bytesRead`, requiring us to reconstruct `input`.
            //ProcessAliases(LineCount);
            input = std::wstring_view{ _buffer };

            // The exact reasons for this are unclear to me (the one writing this comment), but this code used to
            // split the contents of a multiline alias (for instance `doskey test=echo foo$Techo bar$Techo baz`)
            // into multiple separate read outputs, ensuring that the client receives them line by line.
            //
            // This code first truncates the `input` to only contain the first line, so that Consume() below only
            // writes that line into the user buffer. We'll later store the remainder in SaveMultilinePendingInput().
            if (LineCount > 1)
            {
                // ProcessAliases() is supposed to end each line with \r\n. If it doesn't we might as well fail-fast.
                const auto firstLineEnd = input.find(UNICODE_LINEFEED) + 1;
                input = input.substr(0, std::min(input.size(), firstLineEnd));
            }
        }
    }

    const auto inputSizeBefore = input.size();
    GetInputBuffer()->Consume(isUnicode, input, writer);

    if (LineCount > 1)
    {
        // This is a continuation of the above identical if condition.
        // We've truncated the `input` slice and now we need to restore it.
        const auto inputSizeAfter = input.size();
        const auto amountConsumed = inputSizeBefore - inputSizeAfter;
        input = std::wstring_view{ _buffer };
        input = input.substr(std::min(input.size(), amountConsumed));
        GetInputReadHandleData()->SaveMultilinePendingInput(input);
    }
    else if (!input.empty())
    {
        GetInputReadHandleData()->SavePendingInput(input);
    }

    numBytes = _userBuffer.size() - writer.size();
    controlKeyState = _controlKeyState;
}

bool COOKED_READ_DATA::_processInput(const wchar_t wchOrig, const DWORD keyState)
{
    const auto& gci = ServiceLocator::LocateGlobals().getConsoleInformation();
    auto wch = wchOrig;

    if (_ctrlWakeupMask != 0 && wch < L' ' && (_ctrlWakeupMask & (1 << wch)))
    {
        _buffer.push_back(wch);
        _controlKeyState = keyState;
        return true;
    }

    if (wch == EXTKEY_ERASE_PREV_WORD)
    {
        wch = UNICODE_BACKSPACE;
    }

    // in cooked mode, enter (carriage return) is converted to
    // carriage return linefeed (0xda).  carriage return is always
    // stored at the end of the buffer.
    switch (wch)
    {
    case UNICODE_CARRIAGERETURN:
        _buffer.push_back(UNICODE_LINEFEED);
        _insertPosition = _buffer.size();
        break;
    case UNICODE_BACKSPACE:
        _buffer.resize(til::utf16_iterate_prev(_buffer, _buffer.size()));
        _insertPosition = _buffer.size();
        break;
    default:
        _buffer.push_back(wch);
        _insertPosition = _buffer.size();
        break;
    }

    LOG_IF_FAILED(_screenInfo.SetCursorPosition(_positionBeg, true));

    const std::wstring_view view{ _buffer };
    auto length = WriteCharsLegacy(_screenInfo, WC_INTERACTIVE | WC_KEEP_CURSOR_VISIBLE, view.substr(0, _insertPosition));
    const auto cursorPos = _screenInfo.GetTextBuffer().GetCursor().GetPosition();

    length += WriteCharsLegacy(_screenInfo, WC_INTERACTIVE | WC_KEEP_CURSOR_VISIBLE, view.substr(_insertPosition));

    if (_length > length)
    {
        const auto erase = std::min(8, _length - length);
        WriteCharsLegacy(_screenInfo, 0, { L"        ", gsl::narrow_cast<size_t>(erase) });
    }

    LOG_IF_FAILED(_screenInfo.SetCursorPosition(cursorPos, true));

    if (wch == UNICODE_BACKSPACE && _screenInfo.HasAccessibilityEventing())
    {
        if (const auto pConsoleWindow = ServiceLocator::LocateConsoleWindow())
        {
            LOG_IF_FAILED(pConsoleWindow->SignalUia(UIA_Text_TextChangedEventId));
        }
    }
    
    _length = length;
    return wch == UNICODE_CARRIAGERETURN;
}
