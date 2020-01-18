/**
 *  @file
 *  @copyright TODO
*/

#include "boidtoken.hpp"
#include <math.h>
#include <inttypes.h>
#include <stdio.h>

using namespace eosdac;

void boidtoken::create(name issuer, asset maximum_supply)
{
    // accounts perform actions
    // some accounts shouldnt be able to perform specific actions
    // the caller of the action must equal _self
    // _self is the owner of the contract
    require_auth( get_self() );

    // verify valid symbol and max supply is within range of 0 to (1LL<<62)-1
    auto sym = maximum_supply.symbol;
    check(sym.is_valid(), "invalid symbol name");
    check(maximum_supply.is_valid(), "invalid supply");
    check(maximum_supply.amount > 0, "max-supply must be positive");

    // assert we didn't already put some amount of tokens
    // (of type sym) into the contract
    stats statstable(get_self(), sym.code().raw());  // get stats table from EOS database (and declare local version of it)
    auto existing = statstable.find(sym.code().raw());  // and see if sym is already in it
    check(existing == statstable.end(), "symbol already exists");

    // set table so that sym tokens can be issued
    statstable.emplace(get_self(), [&](auto &s) {
        s.supply.symbol = maximum_supply.symbol;
        s.max_supply = maximum_supply;
        s.issuer = issuer;
    });
}

/* Issuer gives tokens to a specified account
 *  - Specified token must be in stats table
 *  - Specified quantity must be less than max supply of token to be issued
 *    -- Max supply from contract-local stats table
 *    -- Max supply not necessarily total token supply over entire economy
 */
void boidtoken::issue(name to, asset quantity, string memo)
{
    print("issue\n");
    auto sym = quantity.symbol;
    check(sym.is_valid(), "invalid symbol name");
    check(memo.size() <= 256, "memo has more than 256 bytes");

    stats statstable(get_self(), sym.code().raw());
    auto existing = statstable.find(sym.code().raw());
    check(existing != statstable.end(),
        "symbol does not exist, create token with symbol before issuing said token");
    const auto &st = *existing;

    require_auth(st.issuer);
    check(quantity.is_valid(), "invalid quantity");
    check(quantity.amount > 0, "must issue positive quantity");

    check(quantity.symbol == st.supply.symbol, "symbol precision mismatch");

    check(quantity.amount <= st.max_supply.amount - st.supply.amount,
      "quantity exceeds available supply");

    statstable.modify(st, _self, [&](auto &s) {
        s.supply += quantity;
    });

    add_balance(st.issuer, quantity, st.issuer);

    if (to != st.issuer)
    {
        action(
            permission_level{st.issuer,"active"_n},
            get_self(),
            "transfer"_n,
            std::make_tuple(st.issuer, to, quantity, memo)
        ).send();
    }
}

void boidtoken::recycle(name account, asset quantity)
{
  print("recycle\n");
  auto sym = quantity.symbol;
  check(sym.is_valid(), "invalid symbol name");

  stats statstable(_self, sym.code().raw());
  auto existing = statstable.find(sym.code().raw());
  check(existing != statstable.end(), "symbol does not exist in stats table");
  const auto &st = *existing;

  require_auth(account);
  check(quantity.is_valid(), "invalid quantity");
  check(quantity.amount > 0, "must recycle positive quantity");

  check(quantity.symbol == st.supply.symbol, "symbol precision mismatch");

  power_t p_t(get_self(), account.value);
  auto p_itr = p_t.find(account.value);
  if (p_itr == p_t.end() ||\
      p_itr->total_delegated.symbol != sym)
    sync_total_delegated(account, account);

  asset available = get_balance(account) - get_total_delegated(account, account, false);
  check(quantity <= available,
    "transferring more than available balance");

  sub_balance(account, quantity, account);

  statstable.modify(st, _self, [&](auto &s) {
      s.supply -= quantity;
      if (s.supply.amount < 0) {
        print("Warning: recycle sets   supply below 0. Please check this out. Setting supply to 0");
        s.supply = asset{0, quantity.symbol};
      }
  });
}

void boidtoken::open( const name& owner, const symbol& symbol, const name& ram_payer )
{
   require_auth( ram_payer );

   check( is_account( owner ), "owner account does not exist" );

   auto sym_code_raw = symbol.code().raw();
   stats statstable( get_self(), sym_code_raw );
   const auto& st = statstable.get( sym_code_raw, "symbol does not exist" );
   check( st.supply.symbol == symbol, "symbol precision mismatch" );

   accounts acnts( get_self(), owner.value );
   auto it = acnts.find( sym_code_raw );
   if( it == acnts.end() ) {
      acnts.emplace( ram_payer, [&]( auto& a ){
        a.balance = asset{0, symbol};
      });
   } else if (owner == ram_payer) {
     acnts.modify(it, ram_payer, [&](auto& a) {
     });
   }
}


void boidtoken::close( const name& owner, const symbol& symbol )
{
   require_auth( owner );
   accounts acnts( get_self(), owner.value );
   auto it = acnts.find( symbol.code().raw() );
   check( it != acnts.end(), "Balance row already deleted or never existed. Action won't have any effect." );
   check( it->balance.amount == 0, "Cannot close because the balance is not zero." );
   acnts.erase( it );
}

/* Transfer tokens from one account to another
 *  - Token type must be same as type to-be-staked via this contract
 *  - Both accounts of transfer must be valid
 */
void boidtoken::transfer(name from, name to, asset quantity, string memo)
{
  print("transfer\n");
  check(from != to, "cannot transfer to self");
  require_auth( from );
  check(is_account(to), "to account does not exist");
  auto sym = quantity.symbol;
  stats statstable(_self, sym.code().raw());
  const auto &st = statstable.get(sym.code().raw());

  require_recipient(from);
  require_recipient(to);

  check(quantity.is_valid(), "invalid quantity");
  check(quantity.amount > 0, "must transfer positive quantity");
  check(quantity.symbol == st.supply.symbol, "symbol precision mismatch");
  check(memo.size() <= 256, "memo has more than 256 bytes");

  power_t p_from_t(get_self(), from.value),
          p_to_t(get_self(), to.value);
  auto p_from_itr = p_from_t.find(from.value),
       p_to_itr = p_to_t.find(to.value);
  if (p_from_itr == p_from_t.end() ||\
      p_from_itr->total_delegated.symbol != sym)
    sync_total_delegated(from, from);
  if (from != to &&\
      (p_to_itr == p_to_t.end() ||\
       p_to_itr->total_delegated.symbol != sym))
    sync_total_delegated(to, from);

  asset available = get_balance(from) - get_total_delegated(from, from, false);
  check(quantity <= available,
    "transferring more than available balance");

  // check if custodian staking, TODO: Remove after staking implemented via token contract
  dacdir::dac dac = dacdir::dac_for_symbol(extended_symbol{quantity.symbol, get_self()});
  eosio::name custodian_contract = dac.account_for_type(dacdir::CUSTODIAN);

  if (is_account(custodian_contract)) {
    if (to == custodian_contract) {
      eosio::action(eosio::permission_level{get_self(), "notify"_n},
                    custodian_contract, "capturestake"_n,
                    make_tuple(from, quantity, dac.dac_id))
      .send();
    }
    else {
        print("to is not custodian", custodian_contract, to);
    }
  }
  else {
      print("NOT account", custodian_contract);
  }

  sub_balance(from, quantity, same_payer);
  add_balance(to, quantity, from);
}

/* Transfer tokens from the contract owner account to a user account as staked tokens
 *  - Token type must be same as type to-be-staked via this contract
 *  - user account must be valid
 */
void boidtoken::transtake(
  name from,
  name to,
  asset quantity,
  uint32_t time_limit)
{
  //require_auth(get_self());
  require_auth( from );
  check(from != to, "Cannot do a transfer stake to self");
  config_t c_t (get_self(), get_self().value);

  auto c_itr = c_t.find(0);
  check(c_itr != c_t.end(), "Must first initstats");
  check(c_itr->stakebreak == STAKE_BREAK_ON, "Can only stake from liquid balance during season break");

  // verify valid and positive _stake amount
  auto sym = quantity.symbol;
  stats statstable(_self, sym.code().raw());
  check(quantity.is_valid(), "invalid quantity");
  check(quantity.amount > 0, "must stake positive quantity");

  // verify symbol and minimum stake amount
  const auto &st = statstable.get(sym.code().raw());
  check(quantity.symbol == st.supply.symbol,
      "symbol precision mismatch");

  power_t p_from_t(get_self(), from.value),
          p_to_t(get_self(), to.value);
  auto p_from_itr = p_from_t.find(from.value),
       p_to_itr = p_to_t.find(to.value);
  if (p_from_itr == p_from_t.end() ||\
      p_from_itr->total_delegated.symbol != sym)
    sync_total_delegated(from, from);
  if (from != to &&\
      (p_to_itr == p_to_t.end() ||\
       p_to_itr->total_delegated.symbol != sym))
    sync_total_delegated(to, from);

  asset available = get_balance(from) - get_total_delegated(from, from, false);
  check(quantity <= available,
    "transfer staking more than available balance");

  stake_t s_t(get_self(), to.value);

  auto s_itr = s_t.find(from.value);
  if (s_itr == s_t.end()) {
    check(quantity.amount >= c_itr->min_stake.amount,
    "Must stake minimum amount");
  } else {
    check(
      s_itr->trans_quantity.amount == 0,
      "Already an existing transfer stake to this account"
    );
  }

  accounts accts(get_self(), from.value);
  const auto& acct = accts.get(quantity.symbol.code().raw(),
    "no account object found");

  time_point t = current_time_point();
  microseconds expiration_time = time_limit == 0 ?
    microseconds(0) : t.time_since_epoch() + microseconds(time_limit*MICROSEC_MULT);

  sub_balance(from, quantity, from);
  add_balance(to, quantity, from);
  add_stake(from, to, quantity, expiration_time, from, true);
}

void boidtoken::stakebreak(uint8_t on_switch)
{
  symbol sym = symbol("BOID",4);
  stats statstable(get_self(), sym.code().raw());
  auto existing = statstable.find(sym.code().raw());
  check(existing != statstable.end(),
      "symbol does not exist, create token with symbol before issuing said token");
  const auto &st = *existing;

  require_auth(st.issuer);
  config_t c_t (_self, _self.value);
  auto c_itr = c_t.find(0);
  check(c_itr != c_t.end(), "Must first initstats");

  time_point t = current_time_point();
  c_t.modify(c_itr, _self, [&](auto &c) {
      c.stakebreak = on_switch;

      if (on_switch == STAKE_BREAK_OFF) {
          c.season_start = t.time_since_epoch();
      }
  });
}

/* Stake tokens with a specified account
 *  - Add account to stake table or add amount staked to existing account
 *  - Specify staking period
 *    -- Stake period must be valid staking period offered by this contract
 *    -- Monthly or Quarterly
 *  - Specify amount staked
 *    -- Token type must be same as type to-be-staked via this contract
 */
void boidtoken::stake(
  name from,
  name to,
  asset quantity,
  uint32_t time_limit,
  bool use_staked_balance)
{
  //require_auth(get_self());

  require_auth( from );

  config_t c_t (get_self(), get_self().value);

  auto c_itr = c_t.find(0);
  check(c_itr != c_t.end(), "Must first initstats");

  check( is_account( from ), "from account does not exist" );
  check( is_account( to ), "to account does not exist" );

  // verify valid and positive stake amount
  auto sym = quantity.symbol;
  stats statstable(_self, sym.code().raw());
  check(quantity.is_valid(), "invalid quantity");
  check(quantity.amount > 0, "must stake positive quantity");

  // verify symbol and minimum stake amount
  const auto &st = statstable.get(sym.code().raw());
  check(quantity.symbol == st.supply.symbol,
      "symbol precision mismatch");

  power_t p_from_t(get_self(), from.value),
          p_to_t(get_self(), to.value);
  auto p_from_itr = p_from_t.find(from.value),
       p_to_itr = p_to_t.find(to.value);
  if (p_from_itr == p_from_t.end() ||\
      p_from_itr->total_delegated.symbol != sym)
    sync_total_delegated(from, from);
  if (from != to &&\
      (p_to_itr == p_to_t.end() ||\
       p_to_itr->total_delegated.symbol != sym))
    sync_total_delegated(to, from);

  delegation_t deleg_t(get_self(), from.value);
  auto deleg = deleg_t.find(from.value);
  asset available;

  if (!use_staked_balance) {
    available = get_balance(from) - get_total_delegated(from, from, false);
  } else {
    available = deleg->quantity;
  }

  check(quantity <= available,
    "staking more than available balance");

  stake_t s_t(get_self(), to.value);

  time_point t = current_time_point();
  microseconds curr_time = t.time_since_epoch(),
               expiration_time = time_limit == 0 ?
                  microseconds(0) : curr_time + microseconds(time_limit*MICROSEC_MULT),
               self_expiration_time;

  auto s_itr = s_t.find(from.value);
  if (s_itr == s_t.end()) {
      check(
        quantity.amount >= c_itr->min_stake.amount,
        "Must stake minimum amount"
      );
  } else {
    check(
      expiration_time >= s_itr->expiration,
      "Already an existing stake to this account with later expiration time"
    );
  }

  string memo, account_type;

  // Assert either:
  //  1) has sufficient liquid balance
  //  2) has sufficient staked, undelegated balance && not staking to self
  if (use_staked_balance) {

    check(
      deleg != deleg_t.end() &&
      quantity <= deleg->quantity,
      "staking more than available staked balance"
    );
    check(
      from != to,
      "staking from staked balance to self"
    );
    check(
      deleg->expiration <= expiration_time,
      "Delegation cannot cause tokens to expire earlier than expected"
    );
    //TODO check if stake exists already
    sub_stake(from, from, quantity, deleg->expiration, from, false);
    add_stake(from, to, quantity, expiration_time, from, false);
    account_type = "stake";
  } else {
    accounts accts(get_self(), from.value);
    const auto& acct = accts.get(quantity.symbol.code().raw(),
      "no account object found");

    check(
      c_itr->stakebreak == STAKE_BREAK_ON,
      "Can only stake from liquid balance out of season"
    );
    check(
      quantity <= acct.balance,
      "staking more than available liquid balance"
    );

    //sub_balance(from, quantity, from);
    add_stake(from, to, quantity, expiration_time, from, false);

    account_type = "liquid";
  }

  memo = "account:  " + from.to_string() +\
       " using " + account_type + " balance"\
       "\naction: stake" +\
       "\ndelegate: " + to.to_string() +\
       "\namount: " + quantity.to_string() +\
       "\ntimeout " + std::to_string(time_limit) + " seconds";

  action(
    permission_level{get_self(),"notify"_n},
    get_self(),
    "sendmessage"_n,
    std::make_tuple(get_self(), memo)
  ).send();
}

void boidtoken::sendmessage(name acct, string memo)
{
    require_auth(acct);
    check(memo.size() <= 256, "memo has more than 256 bytes");
    print(memo);
}

/* Claim token-staking bonus for specified account
 */
//TODO transfers foraccount and wpf bonuses
//TODO research 3rd party permission adding for this specific function
void
boidtoken::claim(
  name stake_account,
  float percentage_to_stake,
  bool issuer_claim
)
{
  // print("claim \n");
  config_t c_t(get_self(), get_self().value);
  auto c_itr = c_t.find(0);
  check(c_itr != c_t.end(), "Must first initstats");

  auto sym = c_itr->bonus.symbol;
  float precision_coef = pow(10,sym.precision());

  accounts accts(get_self(), stake_account.value);
  const auto& a_itr = accts.find(sym.code().raw());
  check(a_itr != accts.end(),
    "Must have BOID account");

  stats statstable(get_self(), sym.code().raw());
  auto existing = statstable.find(sym.code().raw());
  check(existing != statstable.end(),
    "symbol does not exist, create token with symbol before using said token");
  const auto &st = *existing;

  if (issuer_claim) {
    require_auth(st.issuer);
  } else {
    require_auth(stake_account);
  }

  asset zerotokens = asset{0,sym},
        total_payout = zerotokens,
        power_payout = zerotokens,
        stake_payout = zerotokens,
        self_stake_payout = zerotokens,
        wpf_payout = zerotokens,
        powered_stake = zerotokens,
        expired_received_tokens = zerotokens,
        expired_delegated_tokens = zerotokens;

  name ram_payer = issuer_claim ? st.issuer : stake_account;

  power_t pow_t(get_self(), stake_account.value);
  auto bp = pow_t.find(stake_account.value);

  if (bp == pow_t.end() ||\
      bp->total_delegated.symbol != sym) {
    sync_total_delegated(stake_account, ram_payer);
  }
  bp = pow_t.find(stake_account.value);


  time_point t = current_time_point();

  microseconds start_time, claim_time, curr_time = t.time_since_epoch(),
               self_expiration, zeroseconds = microseconds(0);

  float boidpower = update_boidpower(
    bp->quantity,
    0,
    (curr_time - bp->prev_bp_update_time).count()
  );

  stake_t s_t(get_self(), stake_account.value);

  auto self_stake = s_t.find(stake_account.value);
  if (self_stake == s_t.end()) {
    self_expiration = microseconds(0);
  } else {
    self_expiration = self_stake->expiration;
  }

  // Find stake bonus
  float powered_stake_amount = fmin(
    c_itr->powered_stake_multiplier*boidpower*precision_coef,
    c_itr->max_powered_stake_ratio*c_itr->total_staked.amount
  );
  print("min: ", c_itr->max_powered_stake_ratio*c_itr->total_staked.amount);
  print("powered stake amount: ", powered_stake_amount);
  powered_stake = asset{
    (int64_t)powered_stake_amount, sym
  };

  asset curr_stake_payout = zerotokens, curr_wpf_payout = zerotokens;
  for (auto it=s_t.begin(); it != s_t.end(); it++) {
    if (it->quantity.amount > 0) {
      claim_for_stake(
        it->quantity,
        powered_stake,
        it->prev_claim_time,
        it->expiration,
        &curr_stake_payout,
        &curr_wpf_payout
      );

      stake_payout += curr_stake_payout;
      wpf_payout += curr_wpf_payout;

      powered_stake -= it->quantity;
      powered_stake = powered_stake < zerotokens ?
        zerotokens : powered_stake;

      stake_t from_stake(get_self(), it->from.value);
      auto from_self_stake = from_stake.find(it->from.value);
      microseconds from_self_expiration, from_self_trans_expiration;
      if (from_self_stake == from_stake.end()) {
        from_self_expiration = microseconds(0);
      } else {
        from_self_expiration = from_self_stake->expiration;
      }

      if (it->expiration != zeroseconds && it->expiration < curr_time && it->from != it->to) {
        sub_stake(it->from, it->to, it->quantity, it->expiration, same_payer, false);
        add_stake(it->from, it->from, it->quantity, from_self_expiration, ram_payer, false);
      }
      s_t.modify(it, same_payer, [&](auto& s) {
        s.prev_claim_time = curr_time;
      });
    }

    if (it->trans_quantity.amount > 0) {
      claim_for_stake(
        it->trans_quantity,
        powered_stake,
        it->trans_prev_claim_time,
        it->trans_expiration,
        &curr_stake_payout,
        &curr_wpf_payout
      );

      stake_payout += curr_stake_payout;
      wpf_payout += curr_wpf_payout;

      powered_stake -= it->trans_quantity;
      powered_stake = powered_stake < zerotokens ?
        zerotokens : powered_stake;

      if (it->trans_expiration != zeroseconds && it->trans_expiration < curr_time && it->from != it->to) {
        sub_stake(it->from, it->to, it->trans_quantity, it->trans_expiration, same_payer, true);
        add_stake(it->to, it->to, it->trans_quantity, self_expiration, ram_payer, false);
      }

      s_t.modify(it, same_payer, [&](auto& s) {
        s.trans_prev_claim_time = curr_time;
      });
    }
  }

  wpf_payout = wpf_payout < c_itr->max_wpf_payout ?
    wpf_payout : c_itr->max_wpf_payout;
  total_payout += stake_payout + wpf_payout;

  // Reclaim expired delegations
  delegation_t deleg_t(get_self(), stake_account.value);

  for (auto it=deleg_t.begin(); it!=deleg_t.end(); it++) {
    if (it->expiration != zeroseconds && it->expiration < curr_time && it->from != it->to) {
      sub_stake(it->from, it->to, it->quantity, it->expiration, same_payer, false);
      add_stake(it->from, it->from, it->quantity, self_expiration, ram_payer, false);
    }
    if (it->trans_expiration != zeroseconds && it->trans_expiration < curr_time && it->from != it->to) {
      microseconds to_self_expiration;
      delegation_t to_deleg_t(get_self(), it->to.value);
      auto to_self_deleg = to_deleg_t.find(it->to.value);
      if (to_self_deleg == to_deleg_t.end()) {
        to_self_expiration = microseconds(0);
      } else {
        to_self_expiration = to_self_deleg->expiration;
      }
      sub_stake(it->from, it->to, it->trans_quantity, it->trans_expiration, same_payer, true);
      add_stake(it->to, it->to, it->trans_quantity, to_self_expiration, ram_payer, false);
    }
  }

  // Find power bonus and update power and claim parameters
  if (a_itr != accts.end()) {
    start_time = bp->prev_claim_time;
    claim_time = curr_time;
    get_power_bonus(
      start_time, claim_time, boidpower,
      &power_payout
    );

    total_payout += power_payout;

    string debugStr = "Payout would cause token supply to exceed maximum\nstake account: " + stake_account.to_string() +\
      "\ntotal payout: " + total_payout.to_string() +\
      "\npower payout: " + power_payout.to_string() +\
      "\nstake payout: " + stake_payout.to_string();

    check(
      total_payout <= existing->max_supply - existing->supply,
      debugStr
    );

    check(
      total_payout.amount >= 0 &&\
      power_payout.amount >= 0 &&\
      stake_payout.amount >= 0 &&\
      wpf_payout.amount >= 0,
      "All payouts must be zero or positive quantities"
    );

    asset self_payout = power_payout + stake_payout;

    /*
    statstable.modify(st, same_payer, [&](auto& s) {
      s.supply += total_payout;
    });

    add_balance(st.issuer, self_payout, ram_payer);
    */

    string memo = "account:  " + stake_account.to_string() +\
       "\naction: claim" +\
       "\nstake bonus: " + stake_payout.to_string() +\
       "\npower bonus: " + power_payout.to_string() +\
       "\nwpf contribution: " + wpf_payout.to_string() +\
       "\npercentage to self stake: " + self_stake_payout.to_string() +\
       "\nreturning " + expired_received_tokens.to_string() + " expired tokens" +\
       "\nreceiving " + expired_delegated_tokens.to_string() + " delegated tokens";

    action(
      permission_level{st.issuer,"active"_n},
      get_self(),
      "issue"_n,
      std::make_tuple(stake_account, self_payout, memo)
    ).send();

    int64_t self_stake_payout_amount =
      (int64_t)(percentage_to_stake/100)*self_payout.amount;
    self_stake_payout += asset{self_stake_payout_amount, sym};
    if (self_stake_payout_amount > 0 ||\
        self_stake_payout_amount >= c_itr->min_stake.amount) {

      add_stake(stake_account, stake_account, self_stake_payout, self_expiration, ram_payer, false);
    }
    if (ram_payer == st.issuer) ram_payer = same_payer;
    pow_t.modify(bp, ram_payer, [&](auto&p) {
      p.prev_claim_time = curr_time;
      p.total_power_bonus += power_payout;
      p.total_stake_bonus += stake_payout;
      p.quantity = boidpower;
      p.prev_bp_update_time = curr_time;
    });

    if (wpf_payout != zerotokens) {
      action(
        permission_level{st.issuer,"active"_n},
        get_self(),
        "issue"_n,
        std::make_tuple(c_itr->worker_proposal_fund_proxy, wpf_payout, memo)
      ).send();
    }

    update_vote_weight(boidpower, stake_account);
    /*
    c_t.modify(c_itr, same_payer, [&](auto& a) {
      a.worker_proposal_fund += wpf_payout;
    });
    */
  }
}

/* Unstake tokens for specified _stake_account
 *  - Unstake tokens for specified _stake_account
 */
 //TODO add in warning if previous stake is old
void boidtoken::unstake(
  name from,
  name to,
  asset quantity,
  uint32_t time_limit,
  bool to_staked_balance,
  bool issuer_unstake,
  bool transfer
)
{
  //require_auth(get_self());

  //check(!to_staked_balance, "unstake to stake balance temporarily unavailable");
  //check(!transfer, "unstake for transfers temporarily unavailable");

  print("unstake\n");
  config_t c_t(get_self(), get_self().value);
  auto c_itr = c_t.find(0);
  check(c_itr != c_t.end(), "Must first initstats");

  auto sym = quantity.symbol;

  // verify symbol
  stats statstable(get_self(), sym.code().raw());
  const auto &st = statstable.get(sym.code().raw());
  check(sym == st.supply.symbol,
      "symbol precision mismatch");

  power_t p_from_t(get_self(), from.value),
          p_to_t(get_self(), to.value);
  auto p_from_itr = p_from_t.find(from.value),
       p_to_itr = p_to_t.find(to.value);
  // Can unstake to liquid or staked account out of season with delegator
  if ((c_itr->stakebreak == STAKE_BREAK_ON || to_staked_balance)\
      && !issuer_unstake\
      && !transfer) {
    require_auth( from );

    if (p_from_itr == p_from_t.end() ||\
        p_from_itr->total_delegated.symbol != sym)
      sync_total_delegated(from, from);
    if (from != to &&\
        (p_to_itr == p_to_t.end() ||\
         p_to_itr->total_delegated.symbol != sym))
      sync_total_delegated(to, from);

  // Can unstake to liquid or staked account at all times as token issuer
  } else if ((c_itr->stakebreak == STAKE_BREAK_ON || to_staked_balance)\
      && !issuer_unstake\
      && transfer) {
    require_auth(to);

    if (p_from_itr == p_from_t.end() ||\
        p_from_itr->total_delegated.symbol != sym)
      sync_total_delegated(from, to);
    if (from != to &&\
        (p_to_itr == p_to_t.end() ||\
         p_to_itr->total_delegated.symbol != sym))
      sync_total_delegated(to, to);

  } else {
    require_auth(st.issuer);

    if (p_from_itr == p_from_t.end() ||\
        p_from_itr->total_delegated.symbol != sym)
      sync_total_delegated(from, get_self());
    if (from != to &&\
        (p_to_itr == p_to_t.end() ||\
         p_to_itr->total_delegated.symbol != sym))
      sync_total_delegated(to, get_self());
  }

  stake_t s_t(get_self(), to.value);
  auto s_itr = s_t.find(from.value);
  check(
    s_itr != s_t.end(),
    "Nothing to unstake"
  );

  check(quantity.is_valid(), "invalid quantity");
  check(quantity.amount > 0,
    "must unstake positive quantity");

  asset previous_stake_amount = transfer ?
    s_itr->trans_quantity : s_itr->quantity;
  asset amount_after = previous_stake_amount - quantity;

  check(
    amount_after >= c_itr->min_stake ||
    amount_after.amount == 0,
    "After unstake, must have nothing staked or a valid amount"
  );

  time_point t = current_time_point();
  microseconds
    curr_expiration_time = transfer ?
      s_itr->trans_expiration : s_itr->expiration,
    curr_time = t.time_since_epoch();
  print("curr time: ", curr_time.count());
  print("expiration time: ", curr_expiration_time.count());
  check(curr_expiration_time < curr_time,
    "Cannot unstake before time limit");

  if (curr_expiration_time != microseconds(0))
    check(
      previous_stake_amount == quantity,
      "Must unstake all tokens for definite-time stake"
    );

  microseconds expiration_time = time_limit == 0 ?
    microseconds(0) : curr_time + microseconds(time_limit*MICROSEC_MULT);

  if (to_staked_balance && to != from)  {
    sub_stake(from, to, quantity, expiration_time, from, transfer);
    if (transfer) {
      delegation_t to_self_deleg_t(get_self(), to.value);
      auto to_self_deleg = to_self_deleg_t.find(to.value);
      microseconds to_self_expiration = to_self_deleg == to_self_deleg_t.end() ?
        microseconds(0) : to_self_deleg->expiration;

      add_stake(to, to, quantity, to_self_expiration, to, false);
    } else {
      delegation_t self_deleg_t(get_self(), from.value);
      auto self_deleg = self_deleg_t.find(from.value);
      microseconds self_expiration = self_deleg == self_deleg_t.end() ?
        microseconds(0) : self_deleg->expiration;

      add_stake(from, from, quantity, self_expiration, from, false);
    }
  } else if (!to_staked_balance) {
    check(
      c_itr->stakebreak == STAKE_BREAK_ON,
      "Can only unstake to liquid balance during season break"
    );
    sub_stake(from, to, quantity, expiration_time, from, transfer);
  }
}

/* Initialize config table
 */
void boidtoken::initstats(bool wpf_reset)
{
  print("initstats\n");
  require_auth( get_self() );
  config_t c_t (get_self(), get_self().value);
  auto c_itr = c_t.find(0);
  auto sym = symbol("BOID",4);
  asset cleartokens = asset{0, sym};

  // verify symbol
  stats statstable(get_self(), sym.code().raw());
  const auto &st = statstable.get(sym.code().raw());
  check(sym == st.supply.symbol,
      "symbol precision mismatch");
  require_auth(st.issuer);

  float precision_coef = pow(10,sym.precision());

  if (c_itr == c_t.end())
  {
      c_t.emplace( get_self(), [&](auto &c) {

          c.config_id = 0;
          c.stakebreak = STAKE_BREAK_ON;

          c.total_season_bonus = cleartokens;

          c.bonus = cleartokens;
          c.total_staked = cleartokens;
          c.active_accounts = 0;

          int64_t min_stake = (int64_t)precision_coef*1e5;
          c.min_stake = asset{min_stake, sym};
          c.power_difficulty = 25e9;
          c.stake_difficulty = 11e10;
          c.powered_stake_multiplier = 100;
          c.power_bonus_max_rate = 5.8e-7;
          c.max_powered_stake_ratio = .05;

          c.max_wpf_payout = asset{(int64_t)(10000*precision_coef), sym};
          c.worker_proposal_fund = cleartokens;
          c.boidpower_decay_rate = 9.9e-8;
          c.boidpower_update_mult = 0.05;
          c.boidpower_const_decay = 100;
      });
  }
  else
  {
      c_t.modify(c_itr, get_self(), [&](auto &c) {

          c.stakebreak = STAKE_BREAK_ON;

          c.total_season_bonus = cleartokens;

          c.bonus = cleartokens;
          c.total_staked = cleartokens;
          c.active_accounts = 0;

          int64_t min_stake = (int64_t)precision_coef*1e5;
          c.min_stake = asset{min_stake, sym};
          c.power_difficulty = 25e9;
          c.stake_difficulty = 11e10;
          c.powered_stake_multiplier = 100;
          c.power_bonus_max_rate = 5.8e-7;
          c.max_powered_stake_ratio = .05;

          c.max_wpf_payout = asset{(int64_t)(10000*precision_coef), sym};
          if (wpf_reset) {
            c.worker_proposal_fund = cleartokens;
          }
          c.boidpower_decay_rate = 9.9e-8;
          c.boidpower_update_mult = 0.05;
          c.boidpower_const_decay = 100;
      });
  }
}

void boidtoken::erasestakes()
{
  require_auth(get_self());
  staketable s_t(get_self(), get_self().value);
  for (auto it=s_t.begin(); it!=s_t.end(); it++) {
    s_t.erase(it);
  }
}

//FIXME how to set total_delegated correctly for existing stakes without total_delegated entries in power table
void boidtoken::updatepower(const name acct, const float boidpower)
{
  check(boidpower >= 0,
    "Can only have zero or positive boidpower");
  //require_auth(permission_level(get_self(), "poweroracle"_n));
  require_auth(get_self());

  power_t p_t(get_self(), acct.value);

  time_point t = current_time_point();
  microseconds curr_time = t.time_since_epoch(),
               zeroseconds = microseconds(0);

  auto sym = symbol("BOID",4);
  asset zerotokens = asset{0, sym};

  auto bp = p_t.find(acct.value);
  if (bp == p_t.end()) {
    p_t.emplace(get_self(), [&](auto& p) {
      p.acct = acct;
      p.prev_bp_update_time = curr_time;
      p.quantity = update_boidpower(
        0,
        boidpower,
        0
      );
      p.total_power_bonus = zerotokens;
      p.total_stake_bonus = zerotokens;
      p.prev_claim_time = zeroseconds;
      p.total_delegated = get_total_delegated(
        acct, get_self(), true);
    });
  } else {
    p_t.modify(bp, same_payer, [&](auto& p) {
      p.quantity = update_boidpower(
        p.quantity,
        boidpower,
        (curr_time - p.prev_bp_update_time).count()
      );
      p.prev_bp_update_time = curr_time;
      if (p.total_delegated.symbol != sym) {
        p.total_delegated = get_total_delegated(
          acct, get_self(), true);;
      }
    });
  }

  update_vote_weight(boidpower, acct);
}

//FIXME how to set total_delegated correctly for existing stakes without total_delegated entries in power table
void boidtoken::setpower(
  const name acct,
  const float boidpower,
  const bool reset_claim_time
)
{
  check(boidpower >= 0,
    "Can only have zero or positive boidpower");
  require_auth(get_self());

  power_t p_t(get_self(), acct.value);

  time_point t = current_time_point();
  microseconds curr_time = t.time_since_epoch(),
               zeroseconds = microseconds(0);

  auto sym = symbol("BOID",4);
  asset zerotokens = asset{0, sym};

  auto bp = p_t.find(acct.value);
  if (bp == p_t.end()) {
    p_t.emplace(get_self(), [&](auto& p) {
      p.acct = acct;
      p.prev_bp_update_time = curr_time;
      p.quantity = boidpower;
      p.total_power_bonus = zerotokens;
      p.total_stake_bonus = zerotokens;
      p.prev_claim_time = curr_time;
      p.total_delegated = get_total_delegated(
        acct, get_self(), true);
    });
  } else {
    p_t.modify(bp, same_payer, [&](auto& p) {
      p.quantity = boidpower;
      p.prev_bp_update_time = curr_time;
      if (reset_claim_time) {
        p.prev_claim_time = curr_time;
      }
      if (p.total_delegated.symbol != sym) {
        p.total_delegated = get_total_delegated(
          acct, get_self(), true);;
      }
    });
  }


  update_vote_weight(boidpower, acct);
}

void boidtoken::matchtotdel(const name account, const asset quantity, bool subtract)
{
  require_auth(get_self());
  if (subtract) {
    sub_total_delegated(account, quantity, get_self());
  } else {
    add_total_delegated(account, quantity, get_self());
  }
}

void boidtoken::synctotdel(const name account)
{
  require_auth(get_self());
  sync_total_delegated(account, get_self());
}

void boidtoken::syncwpf(const asset quantity)
{
  require_auth(get_self());
  config_t c_t (_self, _self.value);
  auto c_itr = c_t.find(0);
  check(c_itr != c_t.end(), "Must first initstats");
  c_t.modify(c_itr, _self, [&](auto &c) {
      c.worker_proposal_fund += quantity;
  });
}

void boidtoken::setstakediff(const float stake_difficulty)
{
  require_auth( get_self() );
  config_t c_t (_self, _self.value);
  auto c_itr = c_t.find(0);
  check(c_itr != c_t.end(), "Must first initstats");
  c_t.modify(c_itr, _self, [&](auto &c) {
      c.stake_difficulty = stake_difficulty;
  });
}

void boidtoken::setpowerdiff(const float power_difficulty)
{
  require_auth( get_self() );
  config_t c_t (_self, _self.value);
  auto c_itr = c_t.find(0);
  check(c_itr != c_t.end(), "Must first initstats");
  c_t.modify(c_itr, _self, [&](auto &c) {
      c.power_difficulty = power_difficulty;
  });
}

void boidtoken::setpowerrate(const float power_bonus_max_rate)
{
  require_auth( get_self() );
  config_t c_t (_self, _self.value);
  auto c_itr = c_t.find(0);
  check(c_itr != c_t.end(), "Must first initstats");
  c_t.modify(c_itr, _self, [&](auto &c) {
      c.power_bonus_max_rate = power_bonus_max_rate;
  });
}

void boidtoken::setpwrstkmul(const float powered_stake_multiplier)
{
  require_auth( get_self() );
  config_t c_t (_self, _self.value);
  auto c_itr = c_t.find(0);
  check(c_itr != c_t.end(), "Must first initstats");
  c_t.modify(c_itr, _self, [&](auto &c) {
      c.powered_stake_multiplier = powered_stake_multiplier;
  });
}

void boidtoken::setminstake(const asset min_stake)
{
    require_auth( get_self() );
    config_t c_t (_self, _self.value);
    auto c_itr = c_t.find(0);
    check(c_itr != c_t.end(), "Must first initstats");
    c_t.modify(c_itr, _self, [&](auto &c) {
      c.min_stake = min_stake;
    });
}

void boidtoken::setmaxpwrstk(const float percentage)
{
  require_auth( get_self() );
  config_t c_t (_self, _self.value);
  auto c_itr = c_t.find(0);
  check(c_itr != c_t.end(), "Must first initstats");
  c_t.modify(c_itr, _self, [&](auto &c) {
      c.max_powered_stake_ratio = percentage/100;
  });
}

void boidtoken::setmaxwpfpay(const asset max_wpf_payout)
{
  require_auth( get_self() );
  config_t c_t (_self, _self.value);
  auto c_itr = c_t.find(0);
  check(c_itr != c_t.end(), "Must first initstats");
  c_t.modify(c_itr, _self, [&](auto &c) {
      c.max_wpf_payout = max_wpf_payout;
  });
}

void boidtoken::setwpfproxy(const name wpf_proxy)
{
  require_auth( get_self() );
  config_t c_t (_self, _self.value);
  auto c_itr = c_t.find(0);
  check(c_itr != c_t.end(), "Must first initstats");
  c_t.modify(c_itr, _self, [&](auto &c) {
      c.worker_proposal_fund_proxy = wpf_proxy;
  });
}

void boidtoken::collectwpf()
{
  require_auth( get_self() );
  config_t c_t (_self, _self.value);
  auto c_itr = c_t.find(0);
  check(c_itr != c_t.end(), "Must first initstats");
  add_balance(
    c_itr->worker_proposal_fund_proxy,
    c_itr->worker_proposal_fund,
    get_self()
  );
  c_t.modify(c_itr, _self, [&](auto &c) {
      c.worker_proposal_fund -= c.worker_proposal_fund;
  });
}

void boidtoken::recyclewpf()
{
  require_auth(get_self());

  config_t c_t (_self, _self.value);
  auto c_itr = c_t.find(0);
  check(c_itr != c_t.end(), "Must first initstats");

  symbol sym = symbol("BOID",4);
  stats statstable(_self, sym.code().raw());
  auto existing = statstable.find(sym.code().raw());
  check(existing != statstable.end(), "symbol does not exist in stats table");
  const auto &st = *existing;

  require_auth(st.issuer);

  statstable.modify(st, _self, [&](auto &s) {
      s.supply -= c_itr->worker_proposal_fund;
      if (s.supply.amount < 0) {
        print("Warning: recycle sets   supply below 0. Please check this out. Setting supply to 0");
        s.supply = asset{0, sym};
      }
  });
  c_t.modify(c_itr, _self, [&](auto &c) {
      c.worker_proposal_fund -= c.worker_proposal_fund;
  });
}

void boidtoken::setbpdecay(const float decay)
{
  require_auth( get_self() );
  config_t c_t (_self, _self.value);
  auto c_itr = c_t.find(0);
  check(c_itr != c_t.end(), "Must first initstats");
  c_t.modify(c_itr, _self, [&](auto &c) {
      c.boidpower_decay_rate = decay;
  });
}

void boidtoken::setbpmult(const float update_mult)
{
  require_auth( get_self() );
  config_t c_t (_self, _self.value);
  auto c_itr = c_t.find(0);
  check(c_itr != c_t.end(), "Must first initstats");
  c_t.modify(c_itr, _self, [&](auto &c) {
      c.boidpower_update_mult = update_mult;
  });
}

void boidtoken::setbpconst(const float const_decay)
{
  require_auth( get_self() );
  config_t c_t (_self, _self.value);
  auto c_itr = c_t.find(0);
  check(c_itr != c_t.end(), "Must first initstats");
  c_t.modify(c_itr, _self, [&](auto &c) {
      c.boidpower_const_decay = const_decay;
  });
}

void boidtoken::resetbonus(const name account)
{
  require_auth( get_self() );
  symbol sym = symbol("BOID",4);
  power_t p_t(_self, account.value);
  auto p_itr = p_t.find(account.value);
  if (p_itr != p_t.end()) {
    p_t.modify(p_itr, same_payer, [&](auto& p) {
      p.total_power_bonus = asset{0, sym};
      p.total_stake_bonus = asset{0, sym};
    });
  }
}

void boidtoken::resetpowtm(const name account, bool current)
{
  require_auth( get_self() );
  power_t p_t(_self, account.value);
  auto p_itr = p_t.find(account.value);
  if (p_itr != p_t.end()) {
    p_t.modify(p_itr, same_payer, [&](auto& p) {
      p.prev_claim_time = microseconds(0);
      p.prev_bp_update_time = microseconds(0);
    });
  }
}

/* Subtract value from specified account
 */
void boidtoken::sub_balance(name owner, asset value, name ram_payer)
{
  // print("sub balance\nowner = "); print(owner.value); print("\n");
  accounts from_acnts(get_self(), owner.value);
  const auto &from = from_acnts.get(value.symbol.code().raw(), "no balance object found");
  check(from.balance.amount >= value.amount, "overdrawn balance");

  // print(payer.value); print("\n");
  from_acnts.modify(from, ram_payer, [&](auto &a) {
      a.balance -= value;
  });
}

//FIXME update to use tokens table
/* Add value to specified account
 */
void boidtoken::add_balance(name owner, asset value, name ram_payer)
{
  // print("add balance\nowner = "); print(owner.value); print("\n");
  accounts to_acnts(get_self(), owner.value);
  auto to = to_acnts.find(value.symbol.code().raw());

  if (to == to_acnts.end())
  {
    // print("emplace\nram_payer = "); print(ram_payer.value); print("\n");
    to_acnts.emplace(ram_payer, [&](auto &a) {
      a.balance = value;
    });
  }
  else
  {
    // print(payer.value); print("\n");
    //if (ram_payer == get_self()) ram_payer = same_payer;
    to_acnts.modify(to, same_payer, [&](auto &a) {
      a.balance += value;
    });
  }
}

void boidtoken::sub_stake(
  name from,
  name to,
  asset quantity,
  microseconds expiration,
  name ram_payer,
  bool transfer
)
{
  delegation_t deleg_t(get_self(), from.value);
  auto deleg = deleg_t.find(to.value);
  check(deleg != deleg_t.end(), "Delegation does not exist");

  stake_t s_t(get_self(), to.value);
  auto to_itr = s_t.find(from.value);
  check(to_itr != s_t.end(), "Stake does not exist");
  asset curr_delegated_to = transfer ?
    to_itr->trans_quantity : to_itr->quantity;
  asset after_delegated_to = curr_delegated_to - quantity;

  config_t c_t(get_self(), get_self().value);
  auto c_itr = c_t.find(0);
  check(
    after_delegated_to >= c_itr->min_stake ||\
    after_delegated_to.amount == 0,
    "Must maintain minimum stake or have zero stake for this delegation"
  );

  if (transfer) {
    sub_total_delegated(to, quantity, to);
  } else {
    sub_total_delegated(from, quantity, from);
  }

  if (after_delegated_to.amount == 0) {
    deleg_t.erase(deleg);
    s_t.erase(to_itr);
    c_t.modify(c_itr, get_self(), [&](auto& c){
      c.active_accounts -= 1;
      c.total_staked -= quantity;
    });
  } else {
    deleg_t.modify(deleg, ram_payer, [&](auto& d) {
      if (transfer) {
        d.trans_quantity = after_delegated_to;
        d.trans_expiration = expiration;
      } else {
        d.quantity = after_delegated_to;
        d.expiration = expiration;
      }
    });
    s_t.modify(to_itr, ram_payer, [&](auto& s) {
      if (transfer) {
        s.trans_quantity = after_delegated_to;
        s.trans_expiration = expiration;
      } else {
        s.quantity = after_delegated_to;
        s.expiration = expiration;
      }
    });
    c_t.modify(c_itr, get_self(), [&](auto& c){
      c.total_staked -= quantity;
    });
  }
}

void boidtoken::add_stake(
  name from,
  name to,
  asset quantity,
  microseconds expiration,
  name ram_payer,
  bool transfer
)
{
  symbol sym = quantity.symbol;
  asset zerotokens = asset{0,sym};

  stake_t s_t(get_self(), to.value);
  auto to_itr = s_t.find(from.value);

  delegation_t deleg_t(get_self(), from.value);
  auto deleg = deleg_t.find(to.value);

  config_t c_t(get_self(), get_self().value);
  auto c_itr = c_t.find(0);
  check(
    c_itr != c_t.end(),
    "Must first initstats"
  );

  time_point t = current_time_point();
  microseconds curr_time = t.time_since_epoch();

  if (deleg == deleg_t.end()) {
    deleg_t.emplace(ram_payer, [&](auto& a) {
      a.from = from;
      a.to = to;
      if (transfer) {
        a.trans_expiration = expiration;
        a.trans_quantity = quantity;
        a.quantity = zerotokens;
      } else {
        a.quantity = quantity;
        a.expiration = expiration;
        a.trans_quantity = zerotokens;
      }
    });
    s_t.emplace(ram_payer, [&](auto& s) {
      s.from = from;
      s.to = to;
      s.my_bonus = zerotokens;
      if (transfer) {
        s.trans_quantity = quantity;
        s.trans_expiration = expiration;
        s.trans_prev_claim_time = microseconds(0);
        s.quantity = zerotokens;
      } else {
        s.quantity = quantity;
        s.expiration = expiration;
        s.prev_claim_time = microseconds(0);
        s.trans_quantity = zerotokens;
      }
      s.prev_claim_time = curr_time;
    });
    c_t.modify(c_itr, get_self(), [&](auto& c) {
      c.active_accounts += 1;
    });
  } else {
    check(
      to_itr != s_t.end(),
      "Entry exists in delegation table but not stake table"
    );
    //sub_balance(from, quantity, same_payer);
    deleg_t.modify(deleg, same_payer, [&](auto& a) {
      if (transfer) {
        a.trans_expiration = expiration;
        a.trans_quantity += quantity;
      } else {
        a.quantity += quantity;
        a.expiration = expiration;
      }
    });
    s_t.modify(to_itr, same_payer, [&](auto& s) {
      if (transfer) {
        s.trans_quantity += quantity;
        s.trans_expiration = expiration;
      } else {
        s.quantity += quantity;
        s.expiration = expiration;
      }
    });
  }
  // book keeping
  c_t.modify(c_itr, _self, [&](auto &c) {
      c.total_staked += quantity;
  });
  if (transfer) {
    add_total_delegated(to, quantity, ram_payer);
  } else {
    add_total_delegated(from, quantity, ram_payer);
  }
}

void boidtoken::claim_for_stake(
  asset staked,
  asset powered_staked,
  microseconds prev_claim_time,
  microseconds expiration,
  asset* stake_payout,
  asset* wpf_payout
)
{
  symbol sym = staked.symbol;
  asset zerotokens = asset{0,sym};

  config_t c_t(get_self(), get_self().value);
  auto c_itr = c_t.find(0);
  check(
    c_itr != c_t.end(),
    "Must first initstats"
  );

  time_point t = current_time_point();
  microseconds curr_time = t.time_since_epoch(),
               start_time,
               claim_time;

  if (prev_claim_time > curr_time) {
    *stake_payout = zerotokens;
    *wpf_payout = zerotokens;
  };
  start_time = prev_claim_time == microseconds(0) ?
    curr_time : prev_claim_time;

  if (expiration == microseconds(0)) {
    claim_time = curr_time;
  } else if (expiration < curr_time) {
    claim_time = expiration;
  } else {
    claim_time = curr_time;
  }

  check(claim_time <= curr_time, "invalid payout date");

  /*
  if (start_time < c_itr->season_start)
    start_time = c_itr->season_start;
  */

  get_stake_bonus(
    start_time, claim_time, staked, powered_staked,
    stake_payout, wpf_payout
  );
}

void boidtoken::get_stake_bonus(
  microseconds start_time,
  microseconds claim_time,
  asset staked,
  asset powered_staked,
  asset* stake_payout,
  asset* wpf_payout
)
{
  config_t c_t(get_self(), get_self().value);
  auto c_itr = c_t.find(0);
  check(
    c_itr != c_t.end(),
    "Must first initstats"
  );

  symbol sym = staked.symbol;
  int64_t precision_coef = pow(10, sym.precision());
  float amt = fmin(
    (float)staked.amount,
    (float)powered_staked.amount
  );

  float wpf_amt = fmax(
    (float)(staked.amount - powered_staked.amount),
    0
  );

  float stake_coef =
    (claim_time - start_time).count()*\
    TIME_MULT/c_itr->stake_difficulty;

  int64_t payout_amount = (int64_t)(amt*stake_coef)/precision_coef;
  int64_t wpf_payout_amount = (int64_t)(wpf_amt*stake_coef)/precision_coef;
  *stake_payout = asset{payout_amount, sym};
  *wpf_payout = asset{wpf_payout_amount, sym};
  *wpf_payout = *wpf_payout > c_itr->max_wpf_payout ?
    c_itr->max_wpf_payout : *wpf_payout;
  //print("staked amount: ", staked.amount);
  //print("powered staked amount: ", powered_staked.amount);
  print("stake coef: ", stake_coef);
  print("wpf amt: ", wpf_amt);
  print("wpf payout: ", wpf_payout->to_string());
  print("stake payout: ", stake_payout->to_string());
}

void boidtoken::get_power_bonus(
  microseconds start_time,
  microseconds claim_time,
  float boidpower,
  asset* power_payout
)
{
  config_t c_t(get_self(), get_self().value);
  auto c_itr = c_t.find(0);
  check(
    c_itr != c_t.end(),
    "Must first initstats"
  );

  symbol sym = symbol("BOID",4);
  int64_t precision_coef = pow(10, sym.precision());
  float power_coef = fmin(
    (float)(boidpower/c_itr->power_difficulty),
    c_itr->power_bonus_max_rate
  );

  if (start_time.count() == 0) {
    start_time = claim_time;
  }

  *power_payout =
    asset{
      (int64_t)(power_coef*(claim_time - start_time).count()*TIME_MULT),
      sym
    };
  print("dt: ", (claim_time - start_time).count());
  print("power coef: ", power_coef);
  print("power payout: ", power_payout->to_string());
}

//TODO get rid of extra stuff after contract fix
void boidtoken::sub_total_delegated(
  name account,
  asset quantity,
  name ram_payer
)
{
  power_t p_t(_self, account.value);
  auto p_itr = p_t.find(account.value);
  symbol sym = symbol("BOID",4);
  check(p_itr != p_t.end() &&\
        p_itr ->total_delegated.symbol == sym,
    "Must first sync total delegated");

  p_t.modify(p_itr, same_payer, [&](auto& p) {
    p.total_delegated -= quantity;
  });
}

//TODO get rid of extra stuff after contract fix
void boidtoken::add_total_delegated(
  name account,
  asset quantity,
  name ram_payer
)
{
  power_t p_t(_self, account.value);
  auto p_itr = p_t.find(account.value);
  symbol sym = symbol("BOID",4);
  check(p_itr != p_t.end() &&\
        p_itr ->total_delegated.symbol == sym,
    "Must first sync total delegated");
  p_t.modify(p_itr, same_payer, [&](auto& p) {
    p.total_delegated += quantity;
  });
}

//TODO remove ram_payer param
asset boidtoken::get_total_delegated(
  name account,
  name ram_payer,
  bool iterate
)
{
  symbol sym = symbol("BOID",4);
  asset zerotokens = asset{0, sym};
  if (!iterate) {
    power_t p_t(_self, account.value);
    auto p_itr = p_t.find(account.value);
    symbol sym = symbol("BOID",4);
    check(p_itr != p_t.end() &&\
          p_itr ->total_delegated.symbol == sym,
      "Must first sync total delegated");
    return p_itr->total_delegated;
  } else {
    delegation_t d_t(get_self(), account.value);
    asset total_delegated = zerotokens;
    for (auto d_itr = d_t.begin(); d_itr != d_t.end(); d_itr++) {
      total_delegated += d_itr->quantity;
    }
    stake_t s_t(get_self(), account.value);
    for (auto s_itr = s_t.begin(); s_itr != s_t.end(); s_itr++) {
      total_delegated += s_itr->trans_quantity;
    }

    return total_delegated;
  }
}

void boidtoken::sync_total_delegated(
  name account,
  name ram_payer
)
{
  asset total_delegated = get_total_delegated(
    account, ram_payer, true);

  power_t p_t(_self, account.value);
  auto p_itr = p_t.find(account.value);

  if (p_itr != p_t.end()) {
    p_t.modify(p_itr, same_payer, [&](auto& p) {
      p.total_delegated = total_delegated;
    });
  } else {
    time_point t = current_time_point();
    microseconds curr_time = t.time_since_epoch(),
                 zeroseconds = microseconds(0);
    asset zerotokens = asset{0, total_delegated.symbol};
    p_t.emplace(ram_payer, [&](auto& p) {
      p.acct = account;
      p.prev_bp_update_time = curr_time;
      p.quantity = 0;
      p.total_power_bonus = zerotokens;
      p.total_stake_bonus = zerotokens;
      p.prev_claim_time = curr_time;
      p.total_delegated = total_delegated;
    });
  }
}

asset boidtoken::get_balance(
  name account
)
{
  accounts accountstable( get_self(), account.value );
  symbol sym = symbol("BOID",4);
  const auto& ac = accountstable.get( sym.code().raw() );
  return ac.balance;
}

void boidtoken::update_vote_weight(float boidpower, name account){
  name custodian_contract = "bo1dcustodia"_n;
  name dac_id = "boidtestdac2"_n;

  config_t c_t(get_self(), get_self().value);
  auto c_itr = c_t.find(0);
  check(c_itr != c_t.end(), "Must first initstats");

  weight_t vw_t(get_self(), dac_id.value);
  auto vw_itr = vw_t.find(account.value);

  uint64_t prev_powered_stake_amount = 0;
  if (vw_itr != vw_t.end()){
    prev_powered_stake_amount = vw_itr->weight;
  }


  symbol sym = symbol("BOID",4);
  int64_t precision_coef = pow(10, sym.precision());

  float powered_stake_amount = fmin(
    c_itr->powered_stake_multiplier*boidpower*precision_coef,
    c_itr->max_powered_stake_ratio*c_itr->total_staked.amount
  );

  asset total_delegated = get_total_delegated(account, account, false);

  // Arbitrary number
  uint64_t prev_weight = (uint64_t)prev_powered_stake_amount;
  uint64_t new_weight = (uint64_t)fmin(powered_stake_amount, (float)total_delegated.amount);
  int64_t weight_delta = (int64_t)(new_weight - prev_weight);

  std::vector<account_weight_delta> weight_deltas = {{account, weight_delta}};
  action(
          permission_level{get_self(), "notify"_n},
          custodian_contract, "weightobsv"_n,
          make_tuple(weight_deltas, dac_id))
      .send();


  // update current weight
  if (vw_itr != vw_t.end()){
    vw_t.modify(vw_itr, same_payer, [&](auto& vw){
        vw.weight = new_weight;
    });
  }
  else {
    vw_t.emplace(get_self(), [&](auto& vw){
        vw.voter = account;
        vw.weight = new_weight;
    });
  }
}



void boidtoken::memberrege(name sender, string agreedterms, name dac_id) {
    // agreedterms is expected to be the member terms document hash
    require_auth(sender);

    memterms memberterms(get_self(), dac_id.value);

    check(memberterms.begin() != memberterms.end(), "ERR::MEMBERREG_NO_VALID_TERMS::No valid member terms found.");

    auto latest_member_terms = (--memberterms.end());
    check(latest_member_terms->hash == agreedterms, "ERR::MEMBERREG_NOT_LATEST_TERMS::Agreed terms isn't the latest.");
    regmembers registeredgmembers = regmembers(get_self(), dac_id.value);

    auto existingMember = registeredgmembers.find(sender.value);
    if (existingMember != registeredgmembers.end()) {
        registeredgmembers.modify(existingMember, sender, [&](member &mem) {
            mem.agreedtermsversion = latest_member_terms->version;
        });
    } else {
        registeredgmembers.emplace(sender, [&](member &mem) {
            mem.sender = sender;
            mem.agreedtermsversion = latest_member_terms->version;
        });
    }
}

void boidtoken::memberunrege(name sender, name dac_id) {
    require_auth(sender);

    dacdir::dac dac = dacdir::dac_for_id(dac_id);
    eosio::name custodian_account = dac.account_for_type(dacdir::CUSTODIAN);

    candidates_table candidatesTable = candidates_table(custodian_account, dac_id.value);
    auto candidateidx = candidatesTable.find(sender.value);
    if (candidateidx != candidatesTable.end()) {
        print("checking for sender account");

        check(candidateidx->is_active != 1,
              "ERR::MEMBERUNREG_ACTIVE_CANDIDATE::An active candidate must resign their nomination as candidate before being able to unregister from the members.");
    }

    regmembers registeredgmembers = regmembers(_self, dac_id.value);

    auto regMember = registeredgmembers.find(sender.value);
    check(regMember != registeredgmembers.end(), "ERR::MEMBERUNREG_MEMBER_NOT_REGISTERED::Member is not registered.");
    registeredgmembers.erase(regMember);
}

void boidtoken::newmemtermse(string terms, string hash, name dac_id) {

    dacdir::dac dac = dacdir::dac_for_id(dac_id);
    eosio::name auth_account = dac.account_for_type(dacdir::AUTH);
    require_auth(auth_account);

    // sample IPFS: QmXjkFQjnD8i8ntmwehoAHBfJEApETx8ebScyVzAHqgjpD
    check(!terms.empty(), "ERR::NEWMEMTERMS_EMPTY_TERMS::Member terms cannot be empty.");
    check(terms.length() <= 256,
          "ERR::NEWMEMTERMS_TERMS_TOO_LONG::Member terms document url should be less than 256 characters long.");

    check(!hash.empty(), "ERR::NEWMEMTERMS_EMPTY_HASH::Member terms document hash cannot be empty.");
    check(hash.length() <= 32,
          "ERR::NEWMEMTERMS_HASH_TOO_LONG::Member terms document hash should be less than 32 characters long.");

    memterms memberterms(_self, dac_id.value);

    // guard against duplicate of latest
    if (memberterms.begin() != memberterms.end()) {
        auto last = --memberterms.end();
        check(!(terms == last->terms && hash == last->hash),
              "ERR::NEWMEMTERMS_DUPLICATE_TERMS::Next member terms cannot be duplicate of the latest.");
    }

    uint64_t next_version = (memberterms.begin() == memberterms.end() ? 0 : (--memberterms.end())->version) + 1;

    memberterms.emplace(auth_account, [&](termsinfo &termsinfo) {
        termsinfo.terms = terms;
        termsinfo.hash = hash;
        termsinfo.version = next_version;
    });
}

void boidtoken::reset(name dac_id) {
    require_auth(get_self());

    weight_t vw_t(get_self(), dac_id.value);
    auto w = vw_t.begin();

    while (w != vw_t.end()){
        w = vw_t.erase(w);
    }
}
