/*++
Copyright (c) Microsoft Corporation
Licensed under the MIT license.

Module Name:
- CommandNumberPopup.hpp

Abstract:
- Popup used for use command number input
- contains code pulled from popup.cpp and cmdline.cpp

Author:
- Austin Diviness (AustDi) 18-Aug-2018
--*/

#pragma once

#include "popup.h"

class CommandNumberPopup final : public Popup
{
public:
    explicit CommandNumberPopup(SCREEN_INFORMATION& screenInfo);

    [[nodiscard]] NTSTATUS Process(COOKED_READ_DATA& cookedReadData) noexcept override;

protected:
    void _DrawContent() override;

private:
    std::wstring _userInput;

#ifdef UNIT_TESTING
    friend class CommandNumberPopupTests;
#endif
};
