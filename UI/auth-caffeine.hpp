#pragma once

#include "auth-oauth.hpp"
#include "caffeine.h"

class CaffeineChat;
class QLineEdit;
class QWidget;
class QString;
class QDialog;

class CaffeineAuth : public OAuthStreamKey {
	Q_OBJECT
	caff_InstanceHandle instance;

	QSharedPointer<CaffeineChat> chat;
	QSharedPointer<QAction> chatMenu;
	bool uiLoaded = false;

	std::string username;

	void TryAuth(
		QLineEdit * u,
		QLineEdit * p,
		QWidget * parent,
		QString const & caffeineStyle,
		QDialog * prompt);
	virtual bool RetryLogin() override;

	virtual void SaveInternal() override;
	virtual bool LoadInternal() override;

	bool GetChannelInfo();

	virtual void LoadUI() override;

public:
	CaffeineAuth(const Def &d);
	virtual ~CaffeineAuth();

	static std::shared_ptr<Auth> Login(QWidget *parent);
};
