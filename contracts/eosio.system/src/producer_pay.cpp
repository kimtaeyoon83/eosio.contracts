#include <eosio.system/eosio.system.hpp>
#include <eosio.token/eosio.token.hpp>

namespace eosiosystem {

   using eosio::current_time_point;
   using eosio::microseconds;
   using eosio::token;

   
   //https://github.com/EOSIO/eos/blob/eb88d033c0abbc481b8a481485ef4218cdaa033a/libraries/chain/controller.cpp
   //node에서 블록 서명시 이벤트 발생 
   //ignore항목 확인 필요 - 대략적으로 파라미터 구조 일치 여부 확인하지 않는다는 말 같음
   void system_contract::onblock( ignore<block_header> ) {
      using namespace eosio;

      require_auth(get_self());
      
      block_timestamp timestamp;
      
      //현재 블록을 생성한 bp 계정 정보 
      name producer;
      
      //tykim: _ds는 요청 데이터인데 timestamp, producer 순으로 데이터 추출 , 위에 ignore<>과 관련 있음 ignore를 사용했기 때문에 데이터 추출 가능 
      _ds >> timestamp >> producer;

      // _gstate2.last_block_num is not used anywhere in the system contract code anymore.
      // Although this field is deprecated, we will continue updating it for now until the last_block_num field
      // is eventually completely removed, at which point this line can be removed.
      //tykim: *deprecated* 사용안함 
      _gstate2.last_block_num = timestamp;

      /** until activated stake crosses this threshold no new rewards are paid */
      //tykim:  _gstate.total_activated_stake += voter->staked; 투표자의 총 stake 량 
      if( _gstate.total_activated_stake < min_activated_stake )
         return;

      if( _gstate.last_pervote_bucket_fill == time_point() )  /// start the presses
         _gstate.last_pervote_bucket_fill = current_time_point();


      /**
       * At startup the initial producer may not be one that is registered / elected
       * and therefore there may be no producer object for them.
       */
      auto prod = _producers.find( producer.value );
      if ( prod != _producers.end() ) {
         //unpaid 블록 갯수 추가 
         _gstate.total_unpaid_blocks++;
         _producers.modify( prod, same_payer, [&](auto& p ) {
               p.unpaid_blocks++;
         });
      }

      /// only update block producers once every minute, block_timestamp is in half seconds
      //tykim: 1분 마다 
      if( timestamp.slot - _gstate.last_producer_schedule_update.slot > 120 ) {
         //tykim: 1분 마다 bp를 선택함 
         update_elected_producers( timestamp );
           
         //blocks_per_day = 24 * 3600 * 2 이것보다 크면 24시간 경과
         //name 경매 관련 24시간 경과 체크 코드 
         if( (timestamp.slot - _gstate.last_name_close.slot) > blocks_per_day ) {
            name_bid_table bids(get_self(), get_self().value);
            auto idx = bids.get_index<"highbid"_n>();
            auto highest = idx.lower_bound( std::numeric_limits<uint64_t>::max()/2 );
            if( highest != idx.end() &&
                highest->high_bid > 0 &&
                (current_time_point() - highest->last_bid_time) > microseconds(useconds_per_day) &&
                _gstate.thresh_activated_stake_time > time_point() &&
                (current_time_point() - _gstate.thresh_activated_stake_time) > microseconds(14 * useconds_per_day)
            ) {
               _gstate.last_name_close = timestamp;
               //channel_namebid_to_rex - rex 관련 코드 확인 필요 
               channel_namebid_to_rex( highest->high_bid );
               idx.modify( highest, same_payer, [&]( auto& b ){
                  b.high_bid = -b.high_bid;
               });
            }
         }
      }
   }

   //bp 보상 요청 
   void system_contract::claimrewards( const name& owner ) {
      require_auth( owner );

      const auto& prod = _producers.get( owner.value );
      check( prod.active(), "producer does not have an active key" );
      
      //15% 이상 투표된 상태에서 보상 받을수 있음
      check( _gstate.total_activated_stake >= min_activated_stake,
                    "cannot claim rewards until the chain is activated (at least 15% of all tokens participate in voting)" );

      const auto ct = current_time_point();
      
      //하루에 보상 신청은 한번만 가능함 
      check( ct - prod.last_claim_time > microseconds(useconds_per_day), "already claimed rewards within past day" );
      
      //현재 토큰 발행량
      const asset token_supply   = token::get_supply(token_account, core_symbol().code() );
      
      //현재 시간 - 이전 bucket 충전 시간 = 차이시간 카운트 ??
      const auto usecs_since_last_fill = (ct - _gstate.last_pervote_bucket_fill).count();
      
      
      //새로운 토큰이 얼마 발행할지 결정과 토큰을 임시 저장하는 계정으로 토큰 전송 
      //이전 bucket 충전 시간과 현재 시간이 차이가 있고, 현재 시간보다  time_point???
      //https://github.com/EOSIO/eosio.cdt/blob/master/libraries/eosiolib/time.hpp
      if( usecs_since_last_fill > 0 && _gstate.last_pervote_bucket_fill > time_point() ) {
         //새로운 토큰 
         //continuous_rate       = 0.04879;  
         //1년 보상 = rate * 총발행량 * 지난 시간 / 1년 시간 
         auto new_tokens = static_cast<int64_t>( (continuous_rate * double(token_supply.amount) * double(usecs_since_last_fill)) / double(useconds_per_year) );

         //inflation_pay_factor  = 5;                // 20% of the inflation // 
         auto to_producers     = new_tokens / inflation_pay_factor;
         
         // 워커프로포잘 펀드(Worker Proposal Fund)에 80%  // BP보상:WP보상 = 1(20%):4(80%) 비율 
         auto to_savings       = new_tokens - to_producers;
         
         // votepay_factor        = 4;                // 25% of the producer pay
         //이름이 votepay_factor이지만 25%는 블록생성 보상 
         auto to_per_block_pay = to_producers / votepay_factor;
         
         //75%가 투표에대한 비율 보상 <-- 이것도 역시 BP가 받는 보상임, 투표자가 받는 보상이 아님 
         auto to_per_vote_pay  = to_producers - to_per_block_pay;
         {
            //토큰 발행
            token::issue_action issue_act{ token_account, { {get_self(), active_permission} } };
            issue_act.send( get_self(), asset(new_tokens, core_symbol()), "issue tokens for producer pay and savings" );
         }
         {
            //토큰 전송
            token::transfer_action transfer_act{ token_account, { {get_self(), active_permission} } };
            
            //saving_account WP Fund 저장
            transfer_act.send( get_self(), saving_account, asset(to_savings, core_symbol()), "unallocated inflation" );
            
            //BP 보상 저장 계정
            transfer_act.send( get_self(), bpay_account, asset(to_per_block_pay, core_symbol()), "fund per-block bucket" );
            
            //투표 보상 저장 계정 
            transfer_act.send( get_self(), vpay_account, asset(to_per_vote_pay, core_symbol()), "fund per-vote bucket" );
         }

         _gstate.pervote_bucket          += to_per_vote_pay;
         _gstate.perblock_bucket         += to_per_block_pay;
         
         //현재 시간으로 변경 
         _gstate.last_pervote_bucket_fill = ct;
      }
      
      
      //

      auto prod2 = _producers2.find( owner.value );

      /// New metric to be used in pervote pay calculation. Instead of vote weight ratio, we combine vote weight and time duration the vote weight has been held into one metric.
      /// pervote pay 계산에 사용될 새로운 지표. 투표 가중치 비율 대신 투표 가중치와 투표 가중치가 하나의 메트릭으로 유지 된 기간을 결합합니다.
      // 마지막 리워드 시간의 + 3일 동안 시간값
      const auto last_claim_plus_3days = prod.last_claim_time + microseconds(3 * useconds_per_day);

      //현재 시간보다 작거나 같으면 true
      bool crossed_threshold       = (last_claim_plus_3days <= ct);
      bool updated_after_threshold = true;
      if ( prod2 != _producers2.end() ) {
         updated_after_threshold = (last_claim_plus_3days <= prod2->last_votepay_share_update);
      } else {
         prod2 = _producers2.emplace( owner, [&]( producer_info2& info  ) {
            info.owner                     = owner;
            info.last_votepay_share_update = ct;
         });
      }

      // Note: updated_after_threshold implies cross_threshold (except if claiming rewards when the producers2 table row did not exist).
      // The exception leads to updated_after_threshold to be treated as true regardless of whether the threshold was crossed.
      // This is okay because in this case the producer will not get paid anything either way.
      // In fact it is desired behavior because the producers votes need to be counted in the global total_producer_votepay_share for the first time.

      int64_t producer_per_block_pay = 0;
      if( _gstate.total_unpaid_blocks > 0 ) {
         producer_per_block_pay = (_gstate.perblock_bucket * prod.unpaid_blocks) / _gstate.total_unpaid_blocks;
      }

      double new_votepay_share = update_producer_votepay_share( prod2,
                                    ct,
                                    updated_after_threshold ? 0.0 : prod.total_votes,
                                    true // reset votepay_share to zero after updating
                                 );

      int64_t producer_per_vote_pay = 0;
      //0
      if( _gstate2.revision > 0 ) {
         double total_votepay_share = update_total_votepay_share( ct );
         if( total_votepay_share > 0 && !crossed_threshold ) {
            producer_per_vote_pay = int64_t((new_votepay_share * _gstate.pervote_bucket) / total_votepay_share);
            if( producer_per_vote_pay > _gstate.pervote_bucket )
               producer_per_vote_pay = _gstate.pervote_bucket;
         }
      } else {
         if( _gstate.total_producer_vote_weight > 0 ) {
            producer_per_vote_pay = int64_t((_gstate.pervote_bucket * prod.total_votes) / _gstate.total_producer_vote_weight);
         }
      }

      if( producer_per_vote_pay < min_pervote_daily_pay ) {
         producer_per_vote_pay = 0;
      }

      _gstate.pervote_bucket      -= producer_per_vote_pay;
      _gstate.perblock_bucket     -= producer_per_block_pay;
      _gstate.total_unpaid_blocks -= prod.unpaid_blocks;

      update_total_votepay_share( ct, -new_votepay_share, (updated_after_threshold ? prod.total_votes : 0.0) );

      _producers.modify( prod, same_payer, [&](auto& p) {
         p.last_claim_time = ct;
         p.unpaid_blocks   = 0;
      });

      if ( producer_per_block_pay > 0 ) {
         token::transfer_action transfer_act{ token_account, { {bpay_account, active_permission}, {owner, active_permission} } };
         transfer_act.send( bpay_account, owner, asset(producer_per_block_pay, core_symbol()), "producer block pay" );
      }
      if ( producer_per_vote_pay > 0 ) {
         token::transfer_action transfer_act{ token_account, { {vpay_account, active_permission}, {owner, active_permission} } };
         transfer_act.send( vpay_account, owner, asset(producer_per_vote_pay, core_symbol()), "producer vote pay" );
      }
   }

} //namespace eosiosystem
