#include "text/TextTables.h"

#include <array>

namespace Text::Tables
{
    namespace
    {
        constexpr std::array kEntries{
            // Settings sections
            Entry{ Id::kSectionConnection, "Подключение" },
            Entry{ Id::kSectionDefaultOutfit, "Облик по умолчанию" },
            Entry{ Id::kSectionUI, "Интерфейс" },
            Entry{ Id::kSectionControls, "Клавиши" },
            Entry{ Id::kSectionAdvanced, "Дополнительно" },

            // Connection settings
            Entry{ Id::kConnectionDisplayName, "Отображаемое имя" },
            Entry{ Id::kConnectionDisplayNameComment, "Видно другим игрокам над вашим персонажем и в чате." },
            Entry{ Id::kConnectionAutoConnect, "Подключаться автоматически" },
            Entry{ Id::kConnectionAutoConnectComment, "Подключается к серверу автоматически после загрузки игры." },
            Entry{ Id::kConnectionCustomServer, "Свой сервер" },
            Entry{ Id::kConnectionCustomServerComment,
                "Подключается к серверу по адресу ниже вместо официального сервера." },
            Entry{ Id::kConnectionServerEndpoint, "Адрес сервера" },
            Entry{ Id::kConnectionServerEndpointComment,
                "Адрес своего сервера, начинающийся с http:// или https://. Используется, только пока включён свой "
                "сервер." },

            // Default outfit settings
            Entry{
                Id::kOutfitComment, "Облик и снаряжение, которые другие видят вместо ваших модов, которых у них нет." },
            Entry{ Id::kOutfitGroupCharacter, "Персонаж" },
            Entry{ Id::kOutfitRace, "Раса" },
            Entry{ Id::kOutfitSex, "Пол" },
            Entry{ Id::kOutfitMale, "Мужской" },
            Entry{ Id::kOutfitFemale, "Женский" },
            Entry{ Id::kOutfitGroupApparel, "Одежда" },
            Entry{ Id::kOutfitHead, "Голова" },
            Entry{ Id::kOutfitBody, "Тело" },
            Entry{ Id::kOutfitHands, "Руки" },
            Entry{ Id::kOutfitAmulet, "Амулет" },
            Entry{ Id::kOutfitFeet, "Ноги" },
            Entry{ Id::kOutfitShield, "Щит" },
            Entry{ Id::kOutfitCirclet, "Венец" },
            Entry{ Id::kOutfitBackpack, "Рюкзак" },
            Entry{ Id::kOutfitGroupWeapons, "Оружие" },
            Entry{ Id::kOutfitSword, "Меч" },
            Entry{ Id::kOutfitDagger, "Кинжал" },
            Entry{ Id::kOutfitWarAxe, "Топор" },
            Entry{ Id::kOutfitMace, "Булава" },
            Entry{ Id::kOutfitGreatsword, "Двуручный меч" },
            Entry{ Id::kOutfitBattleaxe, "Секира или боевой молот" },
            Entry{ Id::kOutfitBow, "Лук" },
            Entry{ Id::kOutfitStaff, "Посох" },
            Entry{ Id::kOutfitCrossbow, "Арбалет" },
            Entry{ Id::kOutfitGroupMiscellaneous, "Разное" },
            Entry{ Id::kOutfitArrow, "Стрела" },
            Entry{ Id::kOutfitBolt, "Болт" },

            // Interface settings
            Entry{ Id::kUIGroupDisplay, "Отображение" },
            Entry{ Id::kUIFontScale, "Размер шрифта" },
            Entry{ Id::kUIFontScaleComment,
                "Размер текста в окне настроек, в чате и в остальном интерфейсе мультиплеера. Окна растут и "
                "уменьшаются вместе с ним." },
            Entry{ Id::kUIAccentColor, "Цвет акцента" },
            Entry{ Id::kUIAccentColorComment,
                "Цвет, которым отмечено то, что можно выбрать или что выбрано, в окне настроек и в чате, в "
                "шестнадцатеричном виде RRGGBB." },
            Entry{ Id::kUIBorderColor, "Цвет рамки" },
            Entry{ Id::kUIBorderColorComment, "Цвет рамки вокруг окна настроек и чата." },
            Entry{ Id::kUIBorderVanilla, "Ванильный" },
            Entry{ Id::kUIBorderAccent, "Акцент" },
            Entry{ Id::kUIBorderThickness, "Толщина рамки" },
            Entry{ Id::kUIBorderThicknessComment, "Толщина рамки вокруг окна настроек и чата." },
            Entry{ Id::kUIGroupPlayers, "Игроки" },
            Entry{ Id::kUINameplates, "Имена над игроками" },
            Entry{
                Id::kUINameplatesComment, "Показывает имена других игроков над их головами, пока они в поле зрения." },
            Entry{ Id::kUINameplateDistance, "Дальность имён" },
            Entry{ Id::kUINameplateDistanceComment,
                "Расстояние в игровых единицах, в пределах которого имена видны над игроками; около 70 на метр." },
            Entry{ Id::kUIGroupChat, "Чат" },
            Entry{ Id::kUICloseChatOnSend, "Закрывать после отправки" },
            Entry{ Id::kUICloseChatOnSendComment,
                "Закрывает чат, когда Enter отправляет строку. Иначе Enter только отправляет, а пустая строка ничего "
                "не делает." },
            Entry{ Id::kUIChatHorizontal, "Положение по горизонтали" },
            Entry{ Id::kUIChatHorizontalComment,
                "Расстояние от левого края чата до левого края экрана как доля ширины экрана. Чат остаётся на "
                "экране." },
            Entry{ Id::kUIChatVertical, "Положение по вертикали" },
            Entry{ Id::kUIChatVerticalComment,
                "Расстояние от нижнего края чата до нижнего края экрана как доля высоты экрана. Чат остаётся на "
                "экране." },
            Entry{ Id::kUIGroupItems, "Предметы" },
            Entry{ Id::kUIItemPreview, "Предпросмотр предметов" },
            Entry{ Id::kUIItemPreviewComment, "Показывает 3D-модель предмета под курсором в списках предметов." },

            // Keybind settings
            Entry{ Id::kControlsOpenSettings, "Открыть настройки" },
            Entry{ Id::kControlsOpenSettingsComment, "Открывает и закрывает окно настроек." },
            Entry{ Id::kControlsOpenChat, "Открыть чат" },
            Entry{ Id::kControlsOpenChatComment, "Открывает чат." },
            Entry{ Id::kControlsManagePlayers, "Управление игроками" },
            Entry{ Id::kControlsManagePlayersComment,
                "Открывает окно со списком ваших друзей и игроков, которых вы заглушили или скрыли." },
            Entry{ Id::kControlsAcceptTrade, "Принять обмен" },
            Entry{ Id::kControlsAcceptTradeComment,
                "Принимает предложение обмена от другого игрока, пока оно ждёт ответа." },
            Entry{ Id::kControlsDeclineTrade, "Отклонить обмен" },
            Entry{ Id::kControlsDeclineTradeComment,
                "Отклоняет предложение обмена от другого игрока или отзывает ваше, пока оно ждёт ответа." },

            // Advanced settings
            Entry{ Id::kAdvancedGroupPerformance, "Производительность" },
            Entry{ Id::kAdvancedViewRadius, "Радиус видимости" },
            Entry{ Id::kAdvancedViewRadiusComment,
                "Расстояние в игровых единицах, в пределах которого видны другие игроки. Применяется при следующем "
                "подключении." },
            Entry{ Id::kAdvancedViewRadiusAuto, "Авто" },
            Entry{ Id::kAdvancedSpawnsPerSecond, "Появлений в секунду" },
            Entry{ Id::kAdvancedSpawnsPerSecondComment,
                "Наибольшее число удалённых игроков и ездовых животных, появляющихся за секунду. Тогда множество "
                "игроков, пришедших разом, появляются за несколько секунд, а не за один долгий кадр. 0 показывает всех "
                "сразу." },
            Entry{ Id::kAdvancedSpawnsUnlimited, "Без ограничений" },
            Entry{ Id::kAdvancedGroupDiagnostics, "Диагностика" },
            Entry{ Id::kAdvancedStatusPanel, "Панель состояния" },
            Entry{ Id::kAdvancedStatusPanelComment,
                "Показывает пинг, сетевой трафик и время работы плагина на кадр в правом верхнем углу." },

            // Key names
            Entry{ Id::kKeySpace, "Пробел" },
            Entry{ Id::kKeyEnter, "Enter" },
            Entry{ Id::kKeyCapsLock, "Caps Lock" },
            Entry{ Id::kKeyInsert, "Insert" },
            Entry{ Id::kKeyDelete, "Delete" },
            Entry{ Id::kKeyHome, "Home" },
            Entry{ Id::kKeyEnd, "End" },
            Entry{ Id::kKeyPageUp, "Page Up" },
            Entry{ Id::kKeyPageDown, "Page Down" },
            Entry{ Id::kKeyUp, "Вверх" },
            Entry{ Id::kKeyDown, "Вниз" },
            Entry{ Id::kKeyLeft, "Влево" },
            Entry{ Id::kKeyRight, "Вправо" },
            Entry{ Id::kKeyPause, "Pause" },
            Entry{ Id::kKeyScrollLock, "Scroll Lock" },
            Entry{ Id::kKeyNumLock, "Num Lock" },
            Entry{ Id::kKeyNumpadEnter, "Num Enter" },

            // Settings window
            Entry{ Id::kSettingsBrand, "Мультиплеер" },
            Entry{ Id::kSettingsDisplayNameHint, "Введите отображаемое имя" },
            Entry{ Id::kSettingsServerEndpointHint, "Введите адрес сервера" },
            Entry{ Id::kSettingsServerEndpointInvalid, "Неверный адрес сервера." },
            Entry{ Id::kSettingsCustomColor, "Свой цвет" },
            Entry{ Id::kSettingsKeyPrompt, "Нажмите клавишу" },
            Entry{ Id::kSettingsKeyUnbound, "Не назначено" },
            Entry{ Id::kSettingsKeyReserved, "Эту клавишу нельзя назначить" },
            Entry{ Id::kSettingsKeyInUse, "{0} уже назначена на «{1}»" },
            Entry{ Id::kSettingsDefaults, "По умолчанию" },
            Entry{ Id::kSettingsRevert, "Отменить" },
            Entry{ Id::kSettingsApply, "Применить" },
            Entry{ Id::kSettingsUnapplied, "Есть непримененные изменения" },
            Entry{ Id::kSettingsSaved, "Сохранено" },
            Entry{ Id::kSettingsSaveFailed, "Не удалось сохранить {}" },

            // Connection controls
            Entry{ Id::kConnectionConnect, "Подключиться" },
            Entry{ Id::kConnectionCancel, "Отмена" },
            Entry{ Id::kConnectionDisconnect, "Отключиться" },
            Entry{ Id::kConnectionConnected, "Подключено" },
            Entry{ Id::kConnectionConnecting, "Подключение..." },
            Entry{ Id::kConnectionDisconnecting, "Отключение..." },
            Entry{ Id::kConnectionDisconnected, "Отключено" },
            Entry{ Id::kConnectionDisconnectToRename, "Отключитесь, чтобы изменить отображаемое имя" },
            Entry{ Id::kConnectionCancelToRename, "Отмените подключение, чтобы изменить отображаемое имя" },
            Entry{ Id::kConnectionNoEndpoint, "Введите адрес своего сервера, чтобы подключиться." },
            Entry{ Id::kConnectionShortName,
                "Имя должно содержать не меньше {} символа.|Имя должно содержать не меньше {} символов.|Имя должно "
                "содержать не меньше {} символов." },

            // Server refusals
            Entry{ Id::kRefusalProtocolMismatch, "Сервер использует другую версию протокола ({})." },
            Entry{ Id::kRefusalDisplayNameTaken, "Это отображаемое имя уже занято другим игроком." },
            Entry{ Id::kRefusalDisplayNameInvalid, "Сервер не принимает это отображаемое имя." },
            Entry{ Id::kRefusalSessionReplaced,
                "Ваша сессия подключилась из другой игры, и это соединение закрылось. Подключитесь снова, чтобы "
                "вернуть её." },
            Entry{ Id::kRefusalServerFull, "Сервер заполнен." },
            Entry{ Id::kRefusalTooManyNewSessions,
                "С вашего IP-адреса к этому серверу недавно подключилось слишком много новых игроков." },

            // Status panel
            Entry{ Id::kStatusPing, "Пинг" },
            Entry{ Id::kStatusPlugin, "Плагин" },

            // Notices
            Entry{ Id::kNoticeConnected, "Подключено к серверу" },
            Entry{ Id::kNoticeFriendOnline, "{} в сети" },

            // Display name hint; {0} is the key that opens the settings, {1} the Connection tab
            Entry{
                Id::kDisplayNameHint, "Нажмите {0} и введите отображаемое имя на вкладке «{1}», чтобы подключиться" },

            // Trade prompt; {} is the other player's name
            Entry{ Id::kTradePromptWaiting, "Ожидание ответа игрока {} на ваше предложение обмена" },
            Entry{ Id::kTradePromptInvited, "{} предлагает вам обмен" },
            Entry{ Id::kTradePromptWithdraw, "Отозвать" },
            Entry{ Id::kTradePromptAccept, "Принять" },
            Entry{ Id::kTradePromptDecline, "Отклонить" },
            Entry{ Id::kTradePromptSeconds, "{} с" },

            // Chat channels and input; {} is a player's name
            Entry{ Id::kChatGlobal, "Общий" },
            Entry{ Id::kChatNearby, "Рядом" },
            Entry{ Id::kChatWhispers, "Личные" },
            Entry{ Id::kChatTo, "Для {}" },
            Entry{ Id::kChatFrom, "От {}" },
            Entry{ Id::kChatGlobalTargetHint, "Отправить всем игрокам на сервере." },
            Entry{ Id::kChatNearbyTargetHint, "Отправить игрокам рядом с вами." },
            Entry{ Id::kChatWhisperTargetHint, "Отправить личное сообщение игроку {}." },
            Entry{ Id::kChatGlobalFilterHint, "Сообщения для всех игроков на сервере." },
            Entry{ Id::kChatNearbyFilterHint, "Сообщения от игроков рядом с вами." },
            Entry{ Id::kChatWhispersFilterHint, "Личные сообщения между вами и другим игроком." },
            Entry{ Id::kChatNoPlayerNamed, "Игрока с таким именем нет в сети" },
            Entry{ Id::kChatInputHint, "Нажмите Tab, чтобы сменить канал" },

            // Manage players window
            Entry{ Id::kPlayersTitle, "Игроки" },
            Entry{ Id::kPlayersFriends, "Друзья" },
            Entry{ Id::kPlayersMuted, "Заглушённые" },
            Entry{ Id::kPlayersVanished, "Скрытые" },
            Entry{ Id::kPlayersRemoveFriend, "Удалить" },
            Entry{ Id::kPlayersUnmute, "Включить звук" },
            Entry{ Id::kPlayersUnvanish, "Показать" },
            Entry{ Id::kPlayersNoFriends, "Вы не добавили друзей на этом сервере." },
            Entry{ Id::kPlayersNoMuted, "Вы никого не заглушили на этом сервере." },
            Entry{ Id::kPlayersNoVanished, "Вы никого не скрыли на этом сервере." },
            Entry{ Id::kPlayersNoMatch, "Нет игрока с таким именем" },
            Entry{ Id::kPlayersOnline, "В сети" },
            Entry{ Id::kPlayersOffline, "Не в сети" },

            // Player menu; {0} is a race, {1} a level
            Entry{ Id::kPlayerMenuLevel, "Уровень {}" },
            Entry{ Id::kPlayerMenuRaceLevel, "{} · Уровень {}" },
            Entry{ Id::kPlayerMenuWhisper, "Личное сообщение" },
            Entry{ Id::kPlayerMenuAcceptTrade, "Принять обмен" },
            Entry{ Id::kPlayerMenuDeclineTrade, "Отклонить обмен" },
            Entry{ Id::kPlayerMenuTrade, "Обмен" },
            Entry{ Id::kPlayerMenuTradeBusy, "Сначала завершите текущий обмен." },
            Entry{ Id::kPlayerMenuTradeHint, "Предложить обмен предметами." },
            Entry{ Id::kPlayerMenuTradeChecking, "Проверка, рядом ли игрок..." },
            Entry{ Id::kPlayerMenuTradeTooFar, "Встаньте рядом с его персонажем, чтобы обменяться." },
            Entry{ Id::kPlayerMenuAddFriend, "Добавить в друзья" },
            Entry{ Id::kPlayerMenuRemoveFriend, "Удалить из друзей" },
            Entry{ Id::kPlayerMenuAddFriendHint, "Держать игрока в списке друзей и узнавать, когда он заходит." },
            Entry{ Id::kPlayerMenuRemoveFriendHint, "Убрать игрока из списка друзей." },
            Entry{ Id::kPlayerMenuMute, "Заглушить" },
            Entry{ Id::kPlayerMenuMuteHint, "Скрыть его сообщения во всех каналах." },
            Entry{ Id::kPlayerMenuUnmuteHint, "Снова показывать его сообщения." },
            Entry{ Id::kPlayerMenuVanish, "Скрыть" },
            Entry{ Id::kPlayerMenuVanishHint, "Убрать его персонажа из вашего мира." },
            Entry{ Id::kPlayerMenuUnvanishHint, "Снова показывать его персонажа в вашем мире." },

            // Chat player list
            Entry{ Id::kChatNotConnected, "Нет подключения к серверу." },
            Entry{ Id::kChatNoOtherPlayers, "Других игроков в сети нет" },
            Entry{ Id::kChatMutedAndVanished, "Заглушён и скрыт" },

            // Shared controls
            Entry{ Id::kSearchHint, "Поиск" },

            // Trade window; {} is the other player's name unless noted
            Entry{ Id::kTradeTitle, "Обмен с {}" },
            Entry{ Id::kTradePeerOffer, "Предложение: {}" },
            Entry{ Id::kTradeOwnOffer, "Ваше предложение" },
            Entry{ Id::kTradeOfferFull, "(заполнено)" },
            Entry{ Id::kTradeAccepted, "Принято" },
            Entry{ Id::kTradeUninspected,
                "Наведите курсор на предметы, чтобы осмотреть их (остался {})|Наведите курсор на предметы, чтобы "
                "осмотреть их (осталось {})|Наведите курсор на предметы, чтобы осмотреть их (осталось {})" },
            Entry{ Id::kTradeCountOf, "из {}" },
            Entry{ Id::kTradeRemoveAll, "Убрать всё" },
            Entry{ Id::kTradeOffer, "Предложить" },
            Entry{ Id::kTradeInventory, "Ваш инвентарь" },
            Entry{ Id::kTradeCheckingMods, "Проверка модов..." },
            Entry{ Id::kTradeNothingToTrade, "Вам нечего предложить для обмена." },
            Entry{ Id::kTradeAccept, "Принять" },
            Entry{ Id::kTradeCancel, "Отмена" },
            Entry{ Id::kTradeCategoryAll, "Все" },
            Entry{ Id::kTradeCategoryWeapons, "Оружие" },
            Entry{ Id::kTradeCategoryApparel, "Одежда" },
            Entry{ Id::kTradeCategoryAmmunition, "Боеприпасы" },
            Entry{ Id::kTradeCategoryPotions, "Зелья" },
            Entry{ Id::kTradeCategoryIngredients, "Ингредиенты" },
            Entry{ Id::kTradeCategoryBooks, "Книги" },
            Entry{ Id::kTradeCategoryScrolls, "Свитки" },
            Entry{ Id::kTradeCategorySoulGems, "Камни душ" },
            Entry{ Id::kTradeCategoryKeys, "Ключи" },
            Entry{ Id::kTradeCategoryMiscellaneous, "Разное" },

            // Item lists and cards
            Entry{ Id::kItemsNoMatch, "Подходящих предметов нет." },
            Entry{ Id::kItemPickerNone, "Нет" },
            Entry{ Id::kItemPickerMismatch, "{} не подходит для этой настройки, поэтому не используется." },
            Entry{ Id::kItemPickerPluginMissing, "{} не загружен, поэтому этот предмет не используется." },
            Entry{ Id::kItemPickerReset, "Сбросить на {}" },
            Entry{ Id::kItemPickerNotLoaded, "{} (не загружен)" },
            Entry{ Id::kItemPickerNotReady, "Предметы появятся в списке, когда игра загрузит свои данные." },
            Entry{ Id::kItemPickerSearchHint, "Поиск по названию или плагину" },
            Entry{ Id::kItemCardDamage, "УРОН" },
            Entry{ Id::kItemCardArmor, "БРОНЯ" },
            Entry{ Id::kItemCardWeight, "ВЕС" },
            Entry{ Id::kItemCardValue, "ЦЕНА" },
            Entry{ Id::kItemCardCharge, "Заряд {} / {}" },
            Entry{ Id::kItemPreviewUnavailable, "Нет предпросмотра" },
            Entry{ Id::kItemPreviewLoading, "Загрузка..." },

            // Trade notices and reasons; {} is the other player's name unless noted
            Entry{ Id::kListSeparator, ", " },
            Entry{ Id::kNotConnected, "Нет подключения к серверу" },
            Entry{ Id::kTradeNothing, "ничего" },
            Entry{ Id::kTradeBlockQuest, "Нужен для задания" },
            Entry{ Id::kTradeBlockPlayerMade, "Создан вами, поэтому в других играх его нет" },
            Entry{ Id::kTradeBlockUnreferenceable, "Его нельзя описать другой игре" },
            Entry{ Id::kTradeRejectMissingPlugin, "У игрока {0} не установлен {1}" },
            Entry{ Id::kTradeRejectPluginVersion, "У игрока {0} другая версия {1}" },
            Entry{ Id::kTradeRejectMissingEffect, "В игре игрока {} нет этого зачарования" },
            Entry{ Id::kTradeRejectBadEnchantment, "Игра игрока {} не может воссоздать это зачарование" },
            Entry{ Id::kTradeCannotOffer, "Нельзя предложить {0}: {1}" },
            Entry{ Id::kTradeBlockerEmpty, "Добавьте предмет или дождитесь, пока {} добавит свой." },
            Entry{ Id::kTradeBlockerSending, "Отправка вашего предложения..." },
            Entry{ Id::kTradeBlockerChecking, "Ожидание, пока игра игрока {} проверит предложения..." },
            Entry{ Id::kTradeOfferTooLarge,
                "Ваше предложение заполнено: эти предметы содержат слишком много данных, чтобы отправить их вместе" },
            Entry{ Id::kTradeItemsMissing, "У вас больше нет: {}" },
            Entry{ Id::kTradeCompleted, "Обмен успешно завершён" },
            Entry{ Id::kTradeClosedBusy, "{} занят другим обменом" },
            Entry{ Id::kTradeClosedUnavailable, "{} сейчас не может обмениваться" },
            Entry{ Id::kTradeClosedOutOfRange, "{} слишком далеко для обмена" },
            Entry{ Id::kTradeClosedEnded, "Обмен с {} завершён" },
            Entry{ Id::kTradeClosedDeclined, "{} отклонил ваше предложение обмена" },
            Entry{ Id::kTradeClosedNoAnswer, "{} не ответил на ваше предложение обмена" },
            Entry{ Id::kTradeClosedOfferExpired, "Предложение обмена от {} истекло" },
            Entry{ Id::kTradeClosedWithdrawn, "{} отозвал предложение обмена" },
            Entry{ Id::kTradeClosedCancelled, "{} отменил обмен" },
            Entry{ Id::kTradeClosedFailed, "Не удалось открыть обмен с {}" },
            Entry{ Id::kTradeFinishCurrent, "Сначала завершите текущий обмен" },
            Entry{ Id::kTradeCannotTradeNow, "Сейчас нельзя обмениваться" },
            Entry{ Id::kTradeYouDeclined, "Вы отклонили предложение обмена от {}" },
            Entry{ Id::kTradeCannotAccept, "Нельзя обмениваться в бою или вне мира" },
            Entry{ Id::kTradeWindowBlocked, "Окно обмена не открылось, пока было открыто другое меню" },
            Entry{ Id::kTradeCombatDeclined, "Вы в бою\nПредложение обмена от {} отклонено" },
            Entry{ Id::kTradeCombatEnded, "Вы в бою\nОбмен с {} завершён" },
            Entry{ Id::kTradeGotNoAnswer, "Обмен с {} остался без ответа" },

            // Chat messages and failures
            Entry{ Id::kUnknownPlayer, "Неизвестный" },
            Entry{ Id::kPlayersSaveFailed,
                "Не удалось сохранить список друзей, заглушённых и скрытых игроков; изменение действует до выхода из "
                "игры" },
            Entry{ Id::kPlayersVanishLimit,
                "На одном сервере можно скрыть не больше {} игрока|На одном сервере можно скрыть не больше {} "
                "игроков|На одном сервере можно скрыть не больше {} игроков" },
            Entry{ Id::kChatWhisperNeedsName, "Введите имя игрока после @, а затем сообщение" },
            Entry{ Id::kChatRateLimited, "Вы отправляете сообщения слишком часто" },
            Entry{ Id::kChatNotInWorld, "Вы не в игровом мире" },
            Entry{ Id::kChatRecipientGone, "Этого игрока больше нет в сети" },
            Entry{ Id::kChatRejected, "Сервер не принял ваше сообщение" },
            Entry{ Id::kChatInvalidText, "В сообщении есть символы, которые нельзя отправить" },
            Entry{ Id::kChatPlayerOffline, "{} не в сети" },
            Entry{ Id::kItemUnknownEffect, "Неизвестный эффект" },
        };
    }

    std::span<const Entry> GetRussian()
    {
        return kEntries;
    }
}
