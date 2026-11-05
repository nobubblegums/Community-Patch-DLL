-------------------------------------------------
-- Empire Overview: popup routing and tab selection
-------------------------------------------------

include( "IconSupport" );

local m_PopupInfo = nil;
local m_CurrentPanel = nil;

local m_Tabs = {
	Cities = { Panel = Controls.EconomicGeneralInfo, Highlight = Controls.GeneralInfoSelectHighlight },
	Happiness = { Panel = Controls.HappinessInfo, Highlight = Controls.HappinessSelectHighlight },
	Military = { Panel = Controls.MilitaryInfo, Highlight = Controls.MilitarySelectHighlight }
};

local function SelectTab(tab)
	for name, info in pairs(m_Tabs) do
		local bHide = name ~= tab;
		info.Panel:SetHide(bHide);
		info.Highlight:SetHide(bHide);
	end
	m_CurrentPanel = m_Tabs[tab].Panel;
end

local function OnClose()
	UIManager:DequeuePopup(ContextPtr);
end
Controls.CloseButton:RegisterCallback(Mouse.eLClick, OnClose);

local function OnPopup(popupInfo)
	local tab;
	if popupInfo.Type == ButtonPopupTypes.BUTTONPOPUP_ECONOMIC_OVERVIEW then
		tab = "Cities";
	elseif popupInfo.Type == ButtonPopupTypes.BUTTONPOPUP_MILITARY_OVERVIEW then
		tab = "Military";
	else
		return;
	end

	if not ContextPtr:IsHidden() then
		if popupInfo.Data1 == 1 then
			OnClose();
		else
			SelectTab(tab);
		end
		return;
	end

	m_PopupInfo = popupInfo;
	SelectTab(tab);
	local priority = popupInfo.Data1 == 1 and PopupPriority.InGameUtmost or
		(tab == "Military" and PopupPriority.MilitaryOverview or PopupPriority.EconOverview);
	UIManager:QueuePopup(ContextPtr, priority);
end
Events.SerialEventGameMessagePopup.Add(OnPopup);

local function InputHandler(uiMsg, wParam)
	if uiMsg == KeyEvents.KeyDown and
		(wParam == Keys.VK_ESCAPE or wParam == Keys.VK_RETURN) then
		OnClose();
		return true;
	end
end
ContextPtr:SetInputHandler(InputHandler);

local function ShowHideHandler(bIsHide, bInitState)
	if bInitState then
		return;
	end
	if not bIsHide then
		UI.incTurnTimerSemaphore();
		CivIconHookup(Game.GetActivePlayer(), 64, Controls.CivIcon,
			Controls.CivIconBG, Controls.CivIconShadow, false, true);
		m_CurrentPanel:SetHide(false);
		Events.SerialEventGameMessagePopupShown(m_PopupInfo);
	else
		UI.decTurnTimerSemaphore();
		if m_PopupInfo then
			Events.SerialEventGameMessagePopupProcessed.CallImmediate(m_PopupInfo.Type, 0);
			m_PopupInfo = nil;
		end
	end
end
ContextPtr:SetShowHideHandler(ShowHideHandler);

Controls.GeneralInfoButton:RegisterCallback(Mouse.eLClick, function() SelectTab("Cities"); end);
Controls.HappinessButton:RegisterCallback(Mouse.eLClick, function() SelectTab("Happiness"); end);
Controls.MilitaryButton:RegisterCallback(Mouse.eLClick, function() SelectTab("Military"); end);

SelectTab("Cities");

if Game.IsOption(GameOptionTypes.GAMEOPTION_NO_HAPPINESS) then
	Controls.HappinessButton:SetDisabled(true);
	Controls.HappinessButton:LocalizeAndSetToolTip("TXT_KEY_TOP_PANEL_HAPPINESS_OFF_TOOLTIP");
end

Events.GameplaySetActivePlayer.Add(OnClose);
