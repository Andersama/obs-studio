#pragma once

#include "auth-oauth.hpp"
#include "caffeine.h"
#include "caffeine-api.h"

class CaffeineChat;

class CaffeineAuth : public OAuthStreamKey {
	Q_OBJECT

	QSharedPointer<CaffeineChat> chat;
	QSharedPointer<QAction> chatMenu;
	bool uiLoaded = false;

	std::string name;
	std::string id;

	virtual bool RetryLogin() override;

	virtual void SaveInternal() override;
	virtual bool LoadInternal() override;

	bool GetChannelInfo(bool allow_retry = true);

	virtual void LoadUI() override;

public:
	CaffeineAuth(const Def &d);

	static std::shared_ptr<Auth> Login(QWidget *parent);
	void SetToken(std::string token);
};
