#include "text/TextTables.h"

#include <array>

namespace Text::Tables
{
    namespace
    {
        constexpr std::array kEntries{
            // Settings sections
            Entry{ Id::kSectionConnection, "Verbindung" },
            Entry{ Id::kSectionDefaultOutfit, "Standardausrüstung" },
            Entry{ Id::kSectionUI, "Oberfläche" },
            Entry{ Id::kSectionControls, "Tastenbelegung" },
            Entry{ Id::kSectionAdvanced, "Erweitert" },

            // Connection settings
            Entry{ Id::kConnectionDisplayName, "Anzeigename" },
            Entry{ Id::kConnectionDisplayNameComment,
                "Wird anderen Spielern über deinem Charakter und im Chat angezeigt." },
            Entry{ Id::kConnectionAutoConnect, "Automatisch verbinden" },
            Entry{ Id::kConnectionAutoConnectComment,
                "Verbindet sich automatisch mit dem Server, sobald das Spiel geladen ist." },
            Entry{ Id::kConnectionCustomServer, "Eigener Server" },
            Entry{ Id::kConnectionCustomServerComment,
                "Verbindet sich mit dem Server unter der URL unten statt mit dem offiziellen Server." },
            Entry{ Id::kConnectionServerEndpoint, "Server-URL" },
            Entry{ Id::kConnectionServerEndpointComment,
                "URL des eigenen Servers, beginnend mit http:// oder https://. Wird nur verwendet, solange der eigene "
                "Server aktiv ist." },

            // Default outfit settings
            Entry{ Id::kOutfitComment,
                "Aussehen und Ausrüstung, die anderen statt deiner gemoddeten Inhalte gezeigt werden, die sie nicht "
                "haben." },
            Entry{ Id::kOutfitGroupCharacter, "Charakter" },
            Entry{ Id::kOutfitRace, "Volk" },
            Entry{ Id::kOutfitSex, "Geschlecht" },
            Entry{ Id::kOutfitMale, "Männlich" },
            Entry{ Id::kOutfitFemale, "Weiblich" },
            Entry{ Id::kOutfitGroupApparel, "Kleidung" },
            Entry{ Id::kOutfitHead, "Kopf" },
            Entry{ Id::kOutfitBody, "Körper" },
            Entry{ Id::kOutfitHands, "Hände" },
            Entry{ Id::kOutfitAmulet, "Amulett" },
            Entry{ Id::kOutfitFeet, "Füße" },
            Entry{ Id::kOutfitShield, "Schild" },
            Entry{ Id::kOutfitCirclet, "Reif" },
            Entry{ Id::kOutfitBackpack, "Rucksack" },
            Entry{ Id::kOutfitGroupWeapons, "Waffen" },
            Entry{ Id::kOutfitSword, "Schwert" },
            Entry{ Id::kOutfitDagger, "Dolch" },
            Entry{ Id::kOutfitWarAxe, "Kriegsaxt" },
            Entry{ Id::kOutfitMace, "Streitkolben" },
            Entry{ Id::kOutfitGreatsword, "Bihänder" },
            Entry{ Id::kOutfitBattleaxe, "Streitaxt oder Kriegshammer" },
            Entry{ Id::kOutfitBow, "Bogen" },
            Entry{ Id::kOutfitStaff, "Stab" },
            Entry{ Id::kOutfitCrossbow, "Armbrust" },
            Entry{ Id::kOutfitGroupMiscellaneous, "Verschiedenes" },
            Entry{ Id::kOutfitArrow, "Pfeil" },
            Entry{ Id::kOutfitBolt, "Bolzen" },

            // Interface settings
            Entry{ Id::kUIGroupDisplay, "Anzeige" },
            Entry{ Id::kUIFontScale, "Schriftgröße" },
            Entry{ Id::kUIFontScaleComment,
                "Größe des Texts im Einstellungsfenster, im Chat und in der übrigen Mehrspieleroberfläche. Fenster "
                "wachsen und schrumpfen mit." },
            Entry{ Id::kUIAccentColor, "Akzentfarbe" },
            Entry{ Id::kUIAccentColorComment,
                "Farbe, die markiert, was im Einstellungsfenster und im Chat ausgewählt werden kann oder ausgewählt "
                "ist, als Hexadezimalwert RRGGBB." },
            Entry{ Id::kUIBorderColor, "Rahmenfarbe" },
            Entry{ Id::kUIBorderColorComment, "Farbe des Rahmens um das Einstellungsfenster und den Chat." },
            Entry{ Id::kUIBorderVanilla, "Vanilla" },
            Entry{ Id::kUIBorderAccent, "Akzent" },
            Entry{ Id::kUIBorderThickness, "Rahmenstärke" },
            Entry{ Id::kUIBorderThicknessComment, "Stärke des Rahmens um das Einstellungsfenster und den Chat." },
            Entry{ Id::kUIGroupPlayers, "Spieler" },
            Entry{ Id::kUINameplates, "Namensschilder" },
            Entry{ Id::kUINameplatesComment,
                "Zeigt die Namen anderer Spieler über ihren Köpfen, solange sie in Sicht sind." },
            Entry{ Id::kUINameplateDistance, "Namensschild-Entfernung" },
            Entry{ Id::kUINameplateDistanceComment,
                "Entfernung in Spieleinheiten, bis zu der Namen über Spielern angezeigt werden, etwa 70 pro Meter." },
            Entry{ Id::kUIGroupChat, "Chat" },
            Entry{ Id::kUICloseChatOnSend, "Beim Senden schließen" },
            Entry{ Id::kUICloseChatOnSendComment,
                "Schließt den Chat, wenn Eingabe eine Zeile sendet. Sonst sendet Eingabe nur, und eine leere Zeile "
                "bewirkt nichts." },
            Entry{ Id::kUIChatHorizontal, "Horizontale Position" },
            Entry{ Id::kUIChatHorizontalComment,
                "Abstand des linken Chatrands vom linken Bildschirmrand, als Anteil der Bildschirmbreite. Der Chat "
                "bleibt auf dem Bildschirm." },
            Entry{ Id::kUIChatVertical, "Vertikale Position" },
            Entry{ Id::kUIChatVerticalComment,
                "Abstand des unteren Chatrands vom unteren Bildschirmrand, als Anteil der Bildschirmhöhe. Der Chat "
                "bleibt auf dem Bildschirm." },
            Entry{ Id::kUIGroupItems, "Gegenstände" },
            Entry{ Id::kUIItemPreview, "Gegenstandsvorschau" },
            Entry{ Id::kUIItemPreviewComment,
                "Zeigt in Gegenstandslisten ein 3D-Modell des Gegenstands unter dem Mauszeiger." },

            // Keybind settings
            Entry{ Id::kControlsOpenSettings, "Einstellungen öffnen" },
            Entry{ Id::kControlsOpenSettingsComment, "Öffnet und schließt das Einstellungsfenster." },
            Entry{ Id::kControlsOpenChat, "Chat öffnen" },
            Entry{ Id::kControlsOpenChatComment, "Öffnet den Chat." },
            Entry{ Id::kControlsManagePlayers, "Spieler verwalten" },
            Entry{ Id::kControlsManagePlayersComment,
                "Öffnet das Fenster mit deinen Freunden und den Spielern, die du stummgeschaltet oder ausgeblendet "
                "hast." },
            Entry{ Id::kControlsAcceptTrade, "Handel annehmen" },
            Entry{ Id::kControlsAcceptTradeComment,
                "Nimmt das Handelsangebot eines anderen Spielers an, solange es auf eine Antwort wartet." },
            Entry{ Id::kControlsDeclineTrade, "Handel ablehnen" },
            Entry{ Id::kControlsDeclineTradeComment,
                "Lehnt das Handelsangebot eines anderen Spielers ab oder zieht dein eigenes zurück, solange es auf "
                "eine Antwort wartet." },

            // Advanced settings
            Entry{ Id::kAdvancedGroupPerformance, "Leistung" },
            Entry{ Id::kAdvancedViewRadius, "Sichtradius" },
            Entry{ Id::kAdvancedViewRadiusComment,
                "Entfernung in Spieleinheiten, bis zu der andere Spieler angezeigt werden. Gilt ab der nächsten "
                "Verbindung." },
            Entry{ Id::kAdvancedViewRadiusAuto, "Auto" },
            Entry{ Id::kAdvancedSpawnsPerSecond, "Erscheinungen pro Sekunde" },
            Entry{ Id::kAdvancedSpawnsPerSecondComment,
                "Höchstzahl der entfernten Spieler und Reittiere, die pro Sekunde erscheinen. Viele gleichzeitig "
                "ankommende Spieler erscheinen so über einige Sekunden statt in einem langen Standbild. 0 lässt alle "
                "auf einmal erscheinen." },
            Entry{ Id::kAdvancedSpawnsUnlimited, "Unbegrenzt" },
            Entry{ Id::kAdvancedGroupDiagnostics, "Diagnose" },
            Entry{ Id::kAdvancedStatusPanel, "Statusanzeige" },
            Entry{ Id::kAdvancedStatusPanelComment,
                "Zeigt Ping, Netzwerkverkehr und die Zeit des Plugins pro Bild in der oberen rechten Ecke." },

            // Key names
            Entry{ Id::kKeySpace, "Leertaste" },
            Entry{ Id::kKeyEnter, "Eingabe" },
            Entry{ Id::kKeyCapsLock, "Feststelltaste" },
            Entry{ Id::kKeyInsert, "Einfg" },
            Entry{ Id::kKeyDelete, "Entf" },
            Entry{ Id::kKeyHome, "Pos1" },
            Entry{ Id::kKeyEnd, "Ende" },
            Entry{ Id::kKeyPageUp, "Bild auf" },
            Entry{ Id::kKeyPageDown, "Bild ab" },
            Entry{ Id::kKeyUp, "Oben" },
            Entry{ Id::kKeyDown, "Unten" },
            Entry{ Id::kKeyLeft, "Links" },
            Entry{ Id::kKeyRight, "Rechts" },
            Entry{ Id::kKeyPause, "Pause" },
            Entry{ Id::kKeyScrollLock, "Rollen" },
            Entry{ Id::kKeyNumLock, "Num" },
            Entry{ Id::kKeyNumpadEnter, "Num Eingabe" },

            // Settings window
            Entry{ Id::kSettingsBrand, "Mehrspieler" },
            Entry{ Id::kSettingsDisplayNameHint, "Anzeigenamen eingeben" },
            Entry{ Id::kSettingsServerEndpointHint, "Server-URL eingeben" },
            Entry{ Id::kSettingsServerEndpointInvalid, "Ungültige Server-URL." },
            Entry{ Id::kSettingsCustomColor, "Eigene Farbe" },
            Entry{ Id::kSettingsKeyPrompt, "Taste drücken" },
            Entry{ Id::kSettingsKeyUnbound, "Nicht belegt" },
            Entry{ Id::kSettingsKeyReserved, "Diese Taste kann nicht belegt werden" },
            Entry{ Id::kSettingsKeyInUse, "{0} ist bereits mit „{1}“ belegt" },
            Entry{ Id::kSettingsDefaults, "Standard" },
            Entry{ Id::kSettingsRevert, "Zurücksetzen" },
            Entry{ Id::kSettingsApply, "Übernehmen" },
            Entry{ Id::kSettingsUnapplied, "Nicht übernommene Änderungen" },
            Entry{ Id::kSettingsSaved, "Gespeichert" },
            Entry{ Id::kSettingsSaveFailed, "{} konnte nicht gespeichert werden" },

            // Connection controls
            Entry{ Id::kConnectionConnect, "Verbinden" },
            Entry{ Id::kConnectionCancel, "Abbrechen" },
            Entry{ Id::kConnectionDisconnect, "Trennen" },
            Entry{ Id::kConnectionConnected, "Verbunden" },
            Entry{ Id::kConnectionConnecting, "Verbinde..." },
            Entry{ Id::kConnectionDisconnecting, "Trenne..." },
            Entry{ Id::kConnectionDisconnected, "Getrennt" },
            Entry{ Id::kConnectionDisconnectToRename, "Trenne die Verbindung, um den Anzeigenamen zu ändern" },
            Entry{ Id::kConnectionCancelToRename, "Brich die Verbindung ab, um den Anzeigenamen zu ändern" },
            Entry{ Id::kConnectionNoEndpoint, "Gib die URL eines eigenen Servers ein, um dich zu verbinden." },
            Entry{ Id::kConnectionShortName,
                "Der Name muss mindestens {} Zeichen lang sein.|Der Name muss mindestens {} Zeichen lang sein." },

            // Server refusals
            Entry{ Id::kRefusalProtocolMismatch, "Der Server verwendet eine andere Protokollversion ({})." },
            Entry{ Id::kRefusalDisplayNameTaken, "Der Anzeigename wird bereits von einem anderen Spieler verwendet." },
            Entry{ Id::kRefusalDisplayNameInvalid, "Der Server akzeptiert diesen Anzeigenamen nicht." },
            Entry{ Id::kRefusalSessionReplaced,
                "Deine Sitzung wurde aus einem anderen Spiel verbunden, was diese Verbindung geschlossen hat. Verbinde "
                "dich erneut, um sie zurückzuholen." },
            Entry{ Id::kRefusalServerFull, "Der Server ist voll." },
            Entry{ Id::kRefusalTooManyNewSessions,
                "Von deiner IP-Adresse sind kürzlich zu viele neue Spieler diesem Server beigetreten." },

            // Status panel
            Entry{ Id::kStatusPing, "Ping" },
            Entry{ Id::kStatusPlugin, "Plugin" },

            // Notices
            Entry{ Id::kNoticeConnected, "Mit dem Server verbunden" },
            Entry{ Id::kNoticeFriendOnline, "{} ist online" },

            // Display name hint; {0} is the key that opens the settings, {1} the Connection tab
            Entry{
                Id::kDisplayNameHint, "Drücke {0} und gib im Reiter {1} einen Anzeigenamen ein, um dich zu verbinden" },

            // Trade prompt; {} is the other player's name
            Entry{ Id::kTradePromptWaiting, "Warte darauf, dass {} auf dein Handelsangebot antwortet" },
            Entry{ Id::kTradePromptInvited, "{} möchte mit dir handeln" },
            Entry{ Id::kTradePromptWithdraw, "Zurückziehen" },
            Entry{ Id::kTradePromptAccept, "Annehmen" },
            Entry{ Id::kTradePromptDecline, "Ablehnen" },
            Entry{ Id::kTradePromptSeconds, "{} s" },

            // Chat channels and input; {} is a player's name
            Entry{ Id::kChatGlobal, "Global" },
            Entry{ Id::kChatNearby, "In der Nähe" },
            Entry{ Id::kChatWhispers, "Flüstern" },
            Entry{ Id::kChatTo, "An {}" },
            Entry{ Id::kChatFrom, "Von {}" },
            Entry{ Id::kChatGlobalTargetHint, "An alle Spieler auf dem Server senden." },
            Entry{ Id::kChatNearbyTargetHint, "An Spieler in deiner Nähe senden." },
            Entry{ Id::kChatWhisperTargetHint, "{} etwas zuflüstern." },
            Entry{ Id::kChatGlobalFilterHint, "Nachrichten an alle Spieler auf dem Server." },
            Entry{ Id::kChatNearbyFilterHint, "Nachrichten von Spielern in deiner Nähe." },
            Entry{ Id::kChatWhispersFilterHint, "Private Nachrichten zwischen dir und einem anderen Spieler." },
            Entry{ Id::kChatNoPlayerNamed, "Kein Spieler mit diesem Namen ist online" },
            Entry{ Id::kChatInputHint, "Tab drücken, um den Kanal zu wechseln" },

            // Manage players window
            Entry{ Id::kPlayersTitle, "Spieler" },
            Entry{ Id::kPlayersFriends, "Freunde" },
            Entry{ Id::kPlayersMuted, "Stummgeschaltet" },
            Entry{ Id::kPlayersVanished, "Ausgeblendet" },
            Entry{ Id::kPlayersRemoveFriend, "Entfernen" },
            Entry{ Id::kPlayersUnmute, "Stummschaltung aufheben" },
            Entry{ Id::kPlayersUnvanish, "Einblenden" },
            Entry{ Id::kPlayersNoFriends, "Du hast auf diesem Server keine Freunde hinzugefügt." },
            Entry{ Id::kPlayersNoMuted, "Du hast auf diesem Server niemanden stummgeschaltet." },
            Entry{ Id::kPlayersNoVanished, "Du hast auf diesem Server niemanden ausgeblendet." },
            Entry{ Id::kPlayersNoMatch, "Kein Spieler mit diesem Namen" },
            Entry{ Id::kPlayersOnline, "Online" },
            Entry{ Id::kPlayersOffline, "Offline" },

            // Player menu; {0} is a race, {1} a level
            Entry{ Id::kPlayerMenuLevel, "Stufe {}" },
            Entry{ Id::kPlayerMenuRaceLevel, "{} · Stufe {}" },
            Entry{ Id::kPlayerMenuWhisper, "Flüstern" },
            Entry{ Id::kPlayerMenuAcceptTrade, "Handel annehmen" },
            Entry{ Id::kPlayerMenuDeclineTrade, "Handel ablehnen" },
            Entry{ Id::kPlayerMenuTrade, "Handeln" },
            Entry{ Id::kPlayerMenuTradeBusy, "Schließe zuerst deinen laufenden Handel ab." },
            Entry{ Id::kPlayerMenuTradeHint, "Biete einen Tausch von Gegenständen an." },
            Entry{ Id::kPlayerMenuTradeChecking, "Prüfe, ob der Spieler in der Nähe ist..." },
            Entry{ Id::kPlayerMenuTradeTooFar, "Stell dich neben den Charakter, um zu handeln." },
            Entry{ Id::kPlayerMenuAddFriend, "Als Freund hinzufügen" },
            Entry{ Id::kPlayerMenuRemoveFriend, "Freund entfernen" },
            Entry{ Id::kPlayerMenuAddFriendHint,
                "In deine Freundesliste aufnehmen und sehen, wann der Spieler online kommt." },
            Entry{ Id::kPlayerMenuRemoveFriendHint, "Aus deiner Freundesliste entfernen." },
            Entry{ Id::kPlayerMenuMute, "Stummschalten" },
            Entry{ Id::kPlayerMenuMuteHint, "Nachrichten des Spielers in allen Kanälen ausblenden." },
            Entry{ Id::kPlayerMenuUnmuteHint, "Nachrichten des Spielers wieder anzeigen." },
            Entry{ Id::kPlayerMenuVanish, "Ausblenden" },
            Entry{ Id::kPlayerMenuVanishHint, "Den Charakter des Spielers aus deiner Welt entfernen." },
            Entry{ Id::kPlayerMenuUnvanishHint, "Den Charakter des Spielers wieder in deiner Welt zeigen." },

            // Chat player list
            Entry{ Id::kChatNotConnected, "Nicht mit dem Server verbunden." },
            Entry{ Id::kChatNoOtherPlayers, "Keine anderen Spieler online" },
            Entry{ Id::kChatMutedAndVanished, "Stummgeschaltet und ausgeblendet" },

            // Shared controls
            Entry{ Id::kSearchHint, "Suchen" },

            // Trade window; {} is the other player's name unless noted
            Entry{ Id::kTradeTitle, "Handel mit {}" },
            Entry{ Id::kTradePeerOffer, "Angebot von {}" },
            Entry{ Id::kTradeOwnOffer, "Dein Angebot" },
            Entry{ Id::kTradeOfferFull, "(voll)" },
            Entry{ Id::kTradeAccepted, "Angenommen" },
            Entry{ Id::kTradeUninspected,
                "Fahre über die Gegenstände, um sie zu prüfen (noch {})|Fahre über die Gegenstände, um sie zu prüfen "
                "(noch {})" },
            Entry{ Id::kTradeCountOf, "von {}" },
            Entry{ Id::kTradeRemoveAll, "Alle entfernen" },
            Entry{ Id::kTradeOffer, "Anbieten" },
            Entry{ Id::kTradeInventory, "Dein Inventar" },
            Entry{ Id::kTradeCheckingMods, "Prüfe Mods..." },
            Entry{ Id::kTradeNothingToTrade, "Du trägst nichts, was du handeln könntest." },
            Entry{ Id::kTradeAccept, "Annehmen" },
            Entry{ Id::kTradeCancel, "Abbrechen" },
            Entry{ Id::kTradeCategoryAll, "Alle" },
            Entry{ Id::kTradeCategoryWeapons, "Waffen" },
            Entry{ Id::kTradeCategoryApparel, "Kleidung" },
            Entry{ Id::kTradeCategoryAmmunition, "Munition" },
            Entry{ Id::kTradeCategoryPotions, "Tränke" },
            Entry{ Id::kTradeCategoryIngredients, "Zutaten" },
            Entry{ Id::kTradeCategoryBooks, "Bücher" },
            Entry{ Id::kTradeCategoryScrolls, "Schriftrollen" },
            Entry{ Id::kTradeCategorySoulGems, "Seelensteine" },
            Entry{ Id::kTradeCategoryKeys, "Schlüssel" },
            Entry{ Id::kTradeCategoryMiscellaneous, "Verschiedenes" },

            // Item lists and cards
            Entry{ Id::kItemsNoMatch, "Kein Gegenstand passt." },
            Entry{ Id::kItemPickerNone, "Keiner" },
            Entry{ Id::kItemPickerMismatch, "{} passt nicht zu dieser Einstellung und wird daher nicht verwendet." },
            Entry{
                Id::kItemPickerPluginMissing, "{} ist nicht geladen, daher wird dieser Gegenstand nicht verwendet." },
            Entry{ Id::kItemPickerReset, "Zurücksetzen auf {}" },
            Entry{ Id::kItemPickerNotLoaded, "{} (nicht geladen)" },
            Entry{
                Id::kItemPickerNotReady, "Gegenstände werden aufgelistet, sobald das Spiel seine Daten geladen hat." },
            Entry{ Id::kItemPickerSearchHint, "Nach Name oder Plugin suchen" },
            Entry{ Id::kItemCardDamage, "SCHADEN" },
            Entry{ Id::kItemCardArmor, "RÜSTUNG" },
            Entry{ Id::kItemCardWeight, "GEWICHT" },
            Entry{ Id::kItemCardValue, "WERT" },
            Entry{ Id::kItemCardCharge, "Ladung {} / {}" },
            Entry{ Id::kItemPreviewUnavailable, "Keine Vorschau" },
            Entry{ Id::kItemPreviewLoading, "Lädt..." },

            // Trade notices and reasons; {} is the other player's name unless noted
            Entry{ Id::kListSeparator, ", " },
            Entry{ Id::kNotConnected, "Nicht mit dem Server verbunden" },
            Entry{ Id::kTradeNothing, "nichts" },
            Entry{ Id::kTradeBlockQuest, "Eine Quest benötigt es" },
            Entry{ Id::kTradeBlockPlayerMade, "Von dir hergestellt, daher haben andere Spiele es nicht" },
            Entry{ Id::kTradeBlockUnreferenceable, "Es kann einem anderen Spiel nicht beschrieben werden" },
            Entry{ Id::kTradeRejectMissingPlugin, "{0} hat {1} nicht installiert" },
            Entry{ Id::kTradeRejectPluginVersion, "{0} hat eine andere Version von {1}" },
            Entry{ Id::kTradeRejectMissingEffect, "Das Spiel von {} hat diese Verzauberung nicht" },
            Entry{ Id::kTradeRejectBadEnchantment, "Das Spiel von {} kann diese Verzauberung nicht nachbilden" },
            Entry{ Id::kTradeCannotOffer, "Du kannst {0} nicht anbieten: {1}" },
            Entry{ Id::kTradeBlockerEmpty, "Füge einen Gegenstand hinzu oder warte, bis {} einen hinzufügt." },
            Entry{ Id::kTradeBlockerSending, "Sende dein Angebot..." },
            Entry{ Id::kTradeBlockerChecking, "Warte, bis das Spiel von {} die Angebote geprüft hat..." },
            Entry{ Id::kTradeOfferTooLarge,
                "Dein Angebot ist voll; diese Gegenstände enthalten zu viele Daten, um sie zusammen zu senden" },
            Entry{ Id::kTradeItemsMissing, "Du hast nicht mehr: {}" },
            Entry{ Id::kTradeCompleted, "Handel erfolgreich" },
            Entry{ Id::kTradeClosedBusy, "{} ist mit einem anderen Handel beschäftigt" },
            Entry{ Id::kTradeClosedUnavailable, "{} kann gerade nicht handeln" },
            Entry{ Id::kTradeClosedOutOfRange, "{} ist zu weit entfernt zum Handeln" },
            Entry{ Id::kTradeClosedEnded, "Der Handel mit {} wurde beendet" },
            Entry{ Id::kTradeClosedDeclined, "{} hat dein Handelsangebot abgelehnt" },
            Entry{ Id::kTradeClosedNoAnswer, "{} hat nicht auf dein Handelsangebot geantwortet" },
            Entry{ Id::kTradeClosedOfferExpired, "Das Handelsangebot von {} ist abgelaufen" },
            Entry{ Id::kTradeClosedWithdrawn, "{} hat das Handelsangebot zurückgezogen" },
            Entry{ Id::kTradeClosedCancelled, "{} hat den Handel abgebrochen" },
            Entry{ Id::kTradeClosedFailed, "Der Handel mit {} konnte nicht geöffnet werden" },
            Entry{ Id::kTradeFinishCurrent, "Schließe zuerst deinen laufenden Handel ab" },
            Entry{ Id::kTradeCannotTradeNow, "Du kannst gerade nicht handeln" },
            Entry{ Id::kTradeYouDeclined, "Du hast das Handelsangebot von {} abgelehnt" },
            Entry{ Id::kTradeCannotAccept, "Du kannst nicht im Kampf oder außerhalb der Welt handeln" },
            Entry{ Id::kTradeWindowBlocked,
                "Das Handelsfenster konnte nicht geöffnet werden, während ein anderes Menü offen war" },
            Entry{ Id::kTradeCombatDeclined, "Du bist im Kampf\nDas Handelsangebot von {} wurde abgelehnt" },
            Entry{ Id::kTradeCombatEnded, "Du bist im Kampf\nDer Handel mit {} wurde beendet" },
            Entry{ Id::kTradeGotNoAnswer, "Der Handel mit {} hat keine Antwort erhalten" },

            // Chat messages and failures
            Entry{ Id::kUnknownPlayer, "Unbekannt" },
            Entry{ Id::kPlayersSaveFailed,
                "Deine Liste der Freunde, stummgeschalteten und ausgeblendeten Spieler konnte nicht gespeichert "
                "werden; die Änderung gilt bis zum Beenden des Spiels" },
            Entry{ Id::kPlayersVanishLimit,
                "Du kannst höchstens {} Spieler pro Server ausblenden|Du kannst höchstens {} Spieler pro Server "
                "ausblenden" },
            Entry{ Id::kChatWhisperNeedsName, "Gib nach @ den Namen eines Spielers ein, dann deine Nachricht" },
            Entry{ Id::kChatRateLimited, "Du sendest Nachrichten zu schnell" },
            Entry{ Id::kChatNotInWorld, "Du bist nicht in der Welt" },
            Entry{ Id::kChatRecipientGone, "Dieser Spieler ist nicht mehr online" },
            Entry{ Id::kChatRejected, "Der Server hat deine Nachricht nicht angenommen" },
            Entry{ Id::kChatInvalidText, "Deine Nachricht enthält Zeichen, die nicht gesendet werden können" },
            Entry{ Id::kChatPlayerOffline, "{} ist offline" },
            Entry{ Id::kItemUnknownEffect, "Unbekannter Effekt" },
        };
    }

    std::span<const Entry> GetGerman()
    {
        return kEntries;
    }
}
