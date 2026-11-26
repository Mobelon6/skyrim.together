import { Overlay } from '@angular/cdk/overlay';
import {
  Component,
  OnInit,
  ViewChild,
  ChangeDetectionStrategy,
} from '@angular/core';
import { TranslocoService } from '@jsverse/transloco';
import { combineLatest, takeUntil } from 'rxjs';
import { environment } from '../../../environments/environment';
import { fadeInOutActiveAnimation } from '../../animations/fade-in-out-active.animation';
import { View } from '../../models/view.enum';
import { ClientService } from '../../services/client.service';
import { DestroyService } from '../../services/destroy.service';
import {
  SettingService,
  fontSizeToPixels,
} from '../../services/setting.service';
import { Sound, SoundService } from '../../services/sound.service';
import { UiRepository } from '../../store/ui.repository';
import { ChatComponent } from '../chat/chat.component';
import { GroupComponent } from '../group/group.component';
import { controlsAnimation } from './controls.animation';
import { notificationsAnimation } from './notifications.animation';
import { map } from 'rxjs/operators';

const REVEAL_EFFECT_DURATION_MS = 10000; // todo: pass value from C++?

@Component({
  selector: 'app-root',
  templateUrl: './root.component.html',
  styleUrls: ['./root.component.scss'],
  animations: [
    controlsAnimation,
    fadeInOutActiveAnimation,
    notificationsAnimation,
  ],
  host: { 'data-app-root-game': environment.game.toString() },
  providers: [DestroyService],
  changeDetection: ChangeDetectionStrategy.Eager,
  standalone: false,
})
export class RootComponent implements OnInit {
  /* ### ENUMS ### */
  readonly RootView = View;

  readonly observerSubtitle$ = this.client.observerSubtitle.asObservable();
  readonly watching$ = this.client.observerMode.asObservable();
  readonly progress$ = this.client.progressOffer.asObservable();
  answerProgress(token: number, accept: boolean): void {
    skyrimtogether.answerProgress(token, accept ? 1 : 0);
  }
  readonly home$ = this.client.home.asObservable();
  chooseHome(token: number, choice: number): void { skyrimtogether.chooseHome(token,choice); }
  readonly trade$ = this.client.trade.asObservable();
  tradeAction(session: number, action: number, row = 0, count = 1): void {
    skyrimtogether.tradeAction(session, action, row | 0, Math.min(9999, Math.max(1, Math.trunc(count || 1))));
  }
  readonly carriage$ = this.client.carriageOffer.asObservable();
  public answerCarriage(rider: number, trip: number, yes: boolean): void {
    skyrimtogether.answerCarriage(rider, trip, yes ? 1 : 0);
    this.client.carriageOffer.next(null);
  }

  view$ = this.uiRepository.view$;
  readonly conversation$ = this.client.partyConversation.asObservable();

  connected$ = this.client.connectionStateChange.asObservable();
  menuOpen$ = this.client.openingMenuChange.asObservable();
  inGame$ = this.client.inGameStateChange.asObservable();
  active$ = this.client.activationStateChange.asObservable();
  connectionInProgress$ =
    this.client.isConnectionInProgressChange.asObservable();
  revealingInProgress$ = false;

  @ViewChild('chat') private chatComp!: ChatComponent;
  @ViewChild(GroupComponent) private groupComponent: GroupComponent;

  public constructor(
    private readonly destroy$: DestroyService,
    private readonly client: ClientService,
    private readonly sound: SoundService,
    private readonly uiRepository: UiRepository,
    private readonly translocoService: TranslocoService,
    private readonly settingService: SettingService,
    public readonly overlay: Overlay, // used for mockup
  ) {
    this.translocoService.setActiveLang(
      this.settingService.settings.language.getValue(),
    );
  }

  public ngOnInit(): void {
    this.onInGameStateSubscription();
    this.onActivationStateSubscription();
    this.onFontSizeSubscription();
    const {travelNpc, travelMap} = this.settingService.settings;
    combineLatest([travelNpc, travelMap, this.connected$]).pipe(takeUntil(this.destroy$))
      .subscribe(([npc, markers, connected]) => {
        if (connected) skyrimtogether.travelPreferences(Number(npc), Number(markers));
      });
  }

  public onInGameStateSubscription() {
    this.client.inGameStateChange
      .pipe(takeUntil(this.destroy$))
      .subscribe(state => {
        if (!state) {
          this.closeView();
        }
      });
  }

  public onActivationStateSubscription() {
    this.client.activationStateChange
      .pipe(takeUntil(this.destroy$))
      .subscribe(state => {
        if (
          this.client.inGameStateChange.getValue() &&
          state &&
          !this.uiRepository.isViewOpen()
        ) {
          setTimeout(() => { if (!this.client.carriageOffer.value && !this.client.progressOffer.value) this.chatComp?.focus(); }, 100);
        }
        if (!state) {
          this.closeView();
        }
      });
  }

  public onFontSizeSubscription() {
    this.settingService.settings.fontSize
      .pipe(
        takeUntil(this.destroy$),
        map(size => fontSizeToPixels[size]),
      )
      .subscribe(size => {
        document.documentElement.setAttribute('style', `font-size: ${size}px;`);
      });
  }

  public setView(view: View | null) {
    this.uiRepository.openView(view);

    if (view) {
      this.sound.play(Sound.Focus);
    } else if (this.chatComp) {
      this.chatComp.focus();
    }
  }

  public closeView() {
    this.uiRepository.openView(null);
  }

  public reconnect(): void {
    this.client.reconnect();
  }

  public revealPlayers(): void {
    if (this.revealingInProgress$) return;

    this.revealingInProgress$ = true;
    setTimeout(() => {
      this.revealingInProgress$ = false;
    }, REVEAL_EFFECT_DURATION_MS);

    this.sound.play(Sound.Focus);
    this.client.revealPlayers();
  }
}
