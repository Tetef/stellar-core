// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/TrustFrame.h"
#include "ledger/AccountFrame.h"
#include "crypto/Base58.h"
#include "crypto/SHA.h"
#include "database/Database.h"
#include "LedgerDelta.h"
#include "util/types.h"

using namespace std;
using namespace soci;

namespace stellar
{
const char* TrustFrame::kSQLCreateStatement1 =
    "CREATE TABLE TrustLines"
    "("
    "accountID     VARCHAR(51)     NOT NULL,"
    "issuer        VARCHAR(51)     NOT NULL,"
    "AlphaNumCurrency   VARCHAR(4) NOT NULL,"
    "tlimit        BIGINT          NOT NULL DEFAULT 0 CHECK (tlimit >= 0),"
    "balance       BIGINT          NOT NULL DEFAULT 0 CHECK (balance >= 0),"
    "flags         INT             NOT NULL,"
    "PRIMARY KEY (accountID, issuer, AlphaNumCurrency)"
    ");";

const char* TrustFrame::kSQLCreateStatement2 =
    "CREATE INDEX accountLines ON TrustLines (accountID);";

TrustFrame::TrustFrame()
    : EntryFrame(TRUSTLINE), mTrustLine(mEntry.trustLine()), mIsIssuer(false)
{
}

TrustFrame::TrustFrame(LedgerEntry const& from)
    : EntryFrame(from), mTrustLine(mEntry.trustLine()), mIsIssuer(false)
{
}

TrustFrame::TrustFrame(TrustFrame const& from) : TrustFrame(from.mEntry)
{
}

TrustFrame& TrustFrame::operator=(TrustFrame const& other)
{
    if (&other != this)
    {
        mTrustLine = other.mTrustLine;
        mKey = other.mKey;
        mKeyCalculated = other.mKeyCalculated;
        mIsIssuer = other.mIsIssuer;
    }
    return *this;
}

void
TrustFrame::getKeyFields(LedgerKey const& key, std::string& base58AccountID,
                         std::string& base58Issuer, std::string& currencyCode)
{
    base58AccountID = toBase58Check(VER_ACCOUNT_ID, key.trustLine().accountID);
    base58Issuer = toBase58Check(VER_ACCOUNT_ID,
                                 key.trustLine().currency.alphaNum().issuer);
    if (base58AccountID == base58Issuer)
        throw std::runtime_error("Issuer's own trustline should not be used "
                                 "outside of OperationFrame");
    currencyCodeToStr(key.trustLine().currency.alphaNum().currencyCode,
                      currencyCode);
}

int64_t
TrustFrame::getBalance() const
{
    assert(isValid());
    return mTrustLine.balance;
}

bool
TrustFrame::isAuthorized() const
{
    return (mTrustLine.flags & AUTHORIZED_FLAG) != 0;
}

void
TrustFrame::setAuthorized(bool authorized)
{
    if (authorized)
    {
        mTrustLine.flags |= AUTHORIZED_FLAG;
    }
    else
    {
        mTrustLine.flags &= ~AUTHORIZED_FLAG;
    }
}

bool
TrustFrame::addBalance(int64_t delta)
{
    if (mIsIssuer)
    {
        return true;
    }
    if (delta == 0)
    {
        return true;
    }
    if (!isAuthorized())
    {
        return false;
    }
    if (mTrustLine.limit < delta + mTrustLine.balance)
    {
        return false;
    }
    if ((delta + mTrustLine.balance) < 0)
    {
        return false;
    }
    mTrustLine.balance += delta;
    return true;
}

int64_t
TrustFrame::getMaxAmountReceive() const
{
    int64_t amount = 0;
    if (mIsIssuer)
    {
        amount = INT64_MAX;
    }
    else if (isAuthorized())
    {
        amount = mTrustLine.limit - mTrustLine.balance;
    }
    return amount;
}

bool
TrustFrame::isValid() const
{
    TrustLineEntry const& tl = mTrustLine;
    bool res = tl.currency.type() != CURRENCY_TYPE_NATIVE;
    res = res && (tl.balance >= 0);
    res = res && (tl.balance <= tl.limit);
    return res;
}

bool
TrustFrame::exists(Database& db, LedgerKey const& key)
{
    std::string b58AccountID, b58Issuer, currencyCode;
    getKeyFields(key, b58AccountID, b58Issuer, currencyCode);
    int exists = 0;
    auto timer = db.getSelectTimer("trust-exists");
    db.getSession() << "SELECT EXISTS (SELECT NULL FROM TrustLines \
             WHERE accountID=:v1 and issuer=:v2 and AlphaNumCurrency=:v3)",
        use(b58AccountID), use(b58Issuer), use(currencyCode), into(exists);
    return exists != 0;
}

void
TrustFrame::storeDelete(LedgerDelta& delta, Database& db) const
{
    storeDelete(delta, db, getKey());
}

void
TrustFrame::storeDelete(LedgerDelta& delta, Database& db, LedgerKey const& key)
{
    std::string b58AccountID, b58Issuer, currencyCode;
    getKeyFields(key, b58AccountID, b58Issuer, currencyCode);

    auto timer = db.getDeleteTimer("trust");
    db.getSession() << "DELETE from TrustLines \
             WHERE accountID=:v1 and issuer=:v2 and AlphaNumCurrency=:v3",
        use(b58AccountID), use(b58Issuer), use(currencyCode);

    delta.deleteEntry(key);
}

void
TrustFrame::storeChange(LedgerDelta& delta, Database& db) const
{
    assert(isValid());

    if (mIsIssuer)
        return;

    std::string b58AccountID, b58Issuer, currencyCode;
    getKeyFields(getKey(), b58AccountID, b58Issuer, currencyCode);

    auto timer = db.getUpdateTimer("trust");
    statement st = (db.getSession().prepare << "UPDATE TrustLines \
              SET balance=:b, tlimit=:tl, flags=:a \
              WHERE accountID=:v1 and issuer=:v2 and AlphaNumCurrency=:v3",
                    use(mTrustLine.balance), use(mTrustLine.limit),
                    use((int)mTrustLine.flags), use(b58AccountID),
                    use(b58Issuer), use(currencyCode));

    st.execute(true);

    if (st.get_affected_rows() != 1)
    {
        throw std::runtime_error("Could not update data in SQL");
    }

    delta.modEntry(*this);
}

void
TrustFrame::storeAdd(LedgerDelta& delta, Database& db) const
{
    assert(isValid());

    if (mIsIssuer)
        return;

    std::string b58AccountID, b58Issuer, currencyCode;
    getKeyFields(getKey(), b58AccountID, b58Issuer, currencyCode);

    auto timer = db.getInsertTimer("trust");
    statement st =
        (db.getSession().prepare
             << "INSERT INTO TrustLines (accountID, issuer, AlphaNumCurrency, tlimit, flags) \
                 VALUES (:v1,:v2,:v3,:v4,:v5)",
         use(b58AccountID), use(b58Issuer), use(currencyCode),
         use(mTrustLine.limit), use((int)mTrustLine.flags));

    st.execute(true);

    if (st.get_affected_rows() != 1)
    {
        throw std::runtime_error("Could not update data in SQL");
    }

    delta.addEntry(*this);
}

static const char* trustLineColumnSelector =
    "SELECT accountID, issuer, AlphaNumCurrency, tlimit,balance,flags FROM "
    "TrustLines";

void
TrustFrame::setAsIssuer(Currency const& issuer)
{
    mIsIssuer = true;
    mTrustLine.accountID = issuer.alphaNum().issuer;
    mTrustLine.flags |= AUTHORIZED_FLAG;
    mTrustLine.balance = INT64_MAX;
    mTrustLine.currency = issuer;
    mTrustLine.limit = INT64_MAX;
}

bool
TrustFrame::loadTrustLine(AccountID const& accountID, Currency const& currency,
                          TrustFrame& retLine, Database& db)
{
    if (accountID == currency.alphaNum().issuer)
    {
        retLine.setAsIssuer(currency);
        return true;
    }

    std::string accStr, issuerStr, currencyStr;

    accStr = toBase58Check(VER_ACCOUNT_ID, accountID);
    currencyCodeToStr(currency.alphaNum().currencyCode, currencyStr);
    issuerStr = toBase58Check(VER_ACCOUNT_ID, currency.alphaNum().issuer);

    session& session = db.getSession();

    details::prepare_temp_type sql =
        (session.prepare << trustLineColumnSelector
                         << " WHERE accountID=:id AND "
                            "issuer=:issuer AND AlphaNumCurrency=:currency",
         use(accStr), use(issuerStr), use(currencyStr));

    bool res = false;

    auto timer = db.getSelectTimer("trust");
    loadLines(sql, [&retLine, &res](TrustFrame const& trust)
              {
                  retLine = trust;
                  res = true;
              });
    return res;
}

bool
TrustFrame::hasIssued(AccountID const& issuerID, Database& db)
{
    std::string accStr;
    accStr = toBase58Check(VER_ACCOUNT_ID, issuerID);

    session& session = db.getSession();

    details::prepare_temp_type sql =
        (session.prepare << "SELECT balance from TrustLines WHERE issuer=:id "
                            "and balance>0 limit 1",
         use(accStr));

    auto timer = db.getSelectTimer("trust");
    int balance = 0;
    statement st = (sql, into(balance));
    st.execute(true);
    if (st.got_data())
    {
        return true;
    }
    return false;
}

void
TrustFrame::loadLines(details::prepare_temp_type& prep,
                      std::function<void(TrustFrame const&)> trustProcessor)
{
    string accountID;
    std::string issuer, currency;

    TrustFrame curTrustLine;

    TrustLineEntry& tl = curTrustLine.mTrustLine;

    statement st = (prep, into(accountID), into(issuer), into(currency),
                    into(tl.limit), into(tl.balance), into(tl.flags));

    st.execute(true);
    while (st.got_data())
    {
        tl.accountID = fromBase58Check256(VER_ACCOUNT_ID, accountID);
        tl.currency.type(CURRENCY_TYPE_ALPHANUM);
        tl.currency.alphaNum().issuer =
            fromBase58Check256(VER_ACCOUNT_ID, issuer);
        strToCurrencyCode(tl.currency.alphaNum().currencyCode, currency);

        assert(curTrustLine.isValid());
        trustProcessor(curTrustLine);

        st.fetch();
    }
}

void
TrustFrame::loadLines(AccountID const& accountID,
                      std::vector<TrustFrame>& retLines, Database& db)
{
    std::string accStr;
    accStr = toBase58Check(VER_ACCOUNT_ID, accountID);

    session& session = db.getSession();

    details::prepare_temp_type sql =
        (session.prepare << trustLineColumnSelector << " WHERE accountID=:id",
         use(accStr));

    auto timer = db.getSelectTimer("trust");
    loadLines(sql, [&retLines](TrustFrame const& cur)
              {
                  retLines.push_back(cur);
              });
}

void
TrustFrame::dropAll(Database& db)
{
    db.getSession() << "DROP TABLE IF EXISTS TrustLines;";
    db.getSession() << kSQLCreateStatement1;
    db.getSession() << kSQLCreateStatement2;
}
}
